#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <functional>
#include <algorithm>
#include <memory>

template<typename T>
struct Param {
    const char *name;

    Param(const char *name) : name(name)  {}
};

template<typename T>
struct Header {
    const char *name;

    Header(const char *name) : name(name)  {}
};

struct PathPart {
    enum Type { Const, Header, Param };
    Type type;
    std::string value;
};


template<typename T>
std::string to_string_helper(const T &val) {
    return std::to_string(val);
}

std::string to_string_helper(std::string &&val) {
    return std::move(val);
}


template<typename T>
T from_string_helper(const std::string &str);

template<>
std::string from_string_helper<std::string>(const std::string &str) {
    return str;
}

template<>
int from_string_helper<int>(const std::string &str) {
    return std::stoi(str);
}

template<int ...> struct seq {};

template<int N, int ...S> struct gen_seq : gen_seq<N-1, N-1, S...> {};

template<int ...S> struct gen_seq<0, S...>{ typedef seq<S...> type; };


struct Request {

    Request(const std::string &&method, std::string &&uri, std::vector<std::pair<std::string,std::string>> &&headers)
        : headers(std::move(headers))
    {
        uri_parts.push_back(std::move(method));

        std::stringstream ss(std::move(uri));
        std::string item;

        std::getline(ss, item, '/');

        while (std::getline(ss, item, '/')) {
            uri_parts.push_back(item);
        }
    }

    const std::string &header(const std::string &name) const {
        auto iter = std::find_if(begin(headers), end(headers),
                                 [&](const std::pair<std::string,std::string> &pair){
            return pair.first == name;
        });
        if (iter == end(headers)) {
            static const std::string empty_str;
            return empty_str;
        }
        else {
            return iter->second;
        }
    }

    std::vector<std::string> uri_parts;
    std::vector<std::pair<std::string,std::string>> headers;
};

class Service {
public:
    template<typename Ret, typename ...Args>
    Service(std::vector<PathPart> &&sig, std::function<Ret(Args...)> &&func) {
        m_impl.reset(new OpsImpl<Ret,Args...>(std::move(sig), std::move(func)));
    }

    Service(Service &&that)
        : m_impl(std::move(that.m_impl)) {}

    std::string operator()(Request &&req) {
        return m_impl->call(std::move(req));
    }

    const std::vector<PathPart> &signature() const { return m_impl->signature; }

    bool signature_matches(const std::vector<std::string> &uri) const {
        auto uri_iter = uri.begin();
        const auto uri_end = uri.end();

        auto sig_iter = signature().begin();
        const auto sig_end = signature().end();

        while (sig_iter != sig_end) {
            if (sig_iter->type == PathPart::Const) {
                if (uri_iter == uri_end || sig_iter->value != *uri_iter) {
                    return false;
                }
                ++sig_iter;
                ++uri_iter;
                continue;
            }
            if (sig_iter->type == PathPart::Param) {
                if (uri_iter == uri_end) {
                    return false;
                }
                ++sig_iter;
                ++uri_iter;
                continue;
            }
            if (sig_iter->type == PathPart::Header) {
                ++sig_iter;
                continue;
            }
        }

        return uri_iter == uri_end && sig_iter == sig_end;
    }

private:
    struct Ops {
        std::vector<PathPart> signature;
        virtual std::string call(Request &&req) = 0;
    };
    std::unique_ptr<Ops> m_impl;

    template<typename Ret, typename ...Args>
    struct OpsImpl : Ops {
        std::function<Ret(Args...)> func;
        OpsImpl(std::vector<PathPart> &&sig, std::function<Ret(Args...)> &&func)
            : func(std::move(func)) {
            signature = std::move(sig);
        }

        std::string call(Request &&req) override {
            std::vector<const std::string*> args;
            for (unsigned s=0, u=0; s<signature.size(); ++s) {
                const auto &part = signature[s];
                if (part.type == PathPart::Param) {
                    args.push_back(&req.uri_parts[u++]);
                }
                else if (part.type == PathPart::Const) {
                    ++u;
                }
                else if (part.type == PathPart::Header) {
                    args.push_back(&req.header(part.value));
                }
            }
            return to_string_helper( call_func( args, typename gen_seq<sizeof...(Args)>::type() ) );
        }

        template<int ...S>
        Ret call_func(const std::vector<const std::string*> &args, seq<S...>)
        {
            return func( from_string_helper<Args>( *args[S] )... );
        }
    };
};

template<typename Ret=void, typename ...Args>
struct ServiceBuilder {
    std::vector<PathPart> signature;

    template<typename F>
    Service operator=(F &&func) {
        typedef std::function<Ret(Args...)> StdFunc;
        return { std::move(signature), StdFunc(std::forward<F>(func)) };
    }
};

template<typename Ret, typename ...Args>
ServiceBuilder<Ret,Args...> operator/(ServiceBuilder<Ret,Args...> &&s, const char *text) {
    ServiceBuilder<Ret,Args...> result { std::move(s.signature) };
    result.signature.push_back({PathPart::Const, text});
    return result;
}

template<typename Ret, typename ...Args, typename T>
ServiceBuilder<Ret,Args..., T> operator/(ServiceBuilder<Ret,Args...> &&s, const Param<T> &&arg) {
    ServiceBuilder<Ret,Args...,T> result { std::move(s.signature) };
    result.signature.push_back({PathPart::Param, arg.name});
    return result;
}

template<typename Ret, typename ...Args, typename T>
ServiceBuilder<Ret,Args..., T> operator/(ServiceBuilder<Ret,Args...> &&s, const Header<T> &&arg) {
    ServiceBuilder<Ret,Args...,T> result { std::move(s.signature) };
    result.signature.push_back({PathPart::Header, arg.name});
    return result;
}

template<typename Ret>
ServiceBuilder<Ret> get() {
    return ServiceBuilder<Ret>{ { { PathPart::Const, "GET" } } };
}


#include <boost/asio.hpp>
#include <boost/algorithm/string/trim.hpp>

struct Server;



std::string server_handle(Server &server, Request &&request);

class session
        : public std::enable_shared_from_this<session>
{
public:
    session(boost::asio::ip::tcp::socket &&socket, Server&server)
        : socket_(std::move(socket)), server_(server)
    {
    }

    void start()
    {
        read_status();
    }

private:
    void read_status()
    {
        auto self(shared_from_this());
        boost::asio::async_read_until(socket_, buffer_, "\r\n", [this, self](boost::system::error_code ec, std::size_t )
        {
            std::string version;
            char sp1, sp2, cr, lf;
            std::istream is(&buffer_);
            is.unsetf(std::ios_base::skipws);
            is >> request_method_ >> sp1 >> request_uri_ >> sp2 >> version >> cr >> lf;

            read_header();
        });
    }

    void read_header()
    {
        auto self(shared_from_this());
        boost::asio::async_read_until(socket_, buffer_, "\r\n", [this, self](boost::system::error_code ec, std::size_t )
        {
            std::string header_line = [this](){
                std::string header_line;
                std::istream is(&buffer_);
                std::getline(is, header_line);
                boost::algorithm::trim(header_line);
                return header_line;
            }();

            if (!header_line.empty()) {
                auto sep = header_line.find(':');
                auto key = header_line.substr(0, sep);
                auto value = header_line.substr(sep+1);

                request_headers_.emplace_back(std::move(key), std::move(value));

                read_header();
            }
            else {
                auto resp = [&]() -> std::string{
                    try {
                        Request req(std::move(request_method_), std::move(request_uri_), std::move(request_headers_));
                        const std::string resp = server_handle(server_, std::move(req));
                        return "HTTP/1.1 200 OK\r\n"
                               "Connection: close\r\n"
                               "Content-Type: text/plain\r\n"
                               "\r\n" + resp;
                    }
                    catch (...) {
                        return "HTTP/1.1 404 Not Found\r\n"
                               "Connection: close\r\n"
                               "\r\n" ;
                    }
                }();


                boost::asio::write(socket_, boost::asio::buffer(resp));
                socket_.close();
            }
        });
    }

    boost::asio::ip::tcp::socket socket_;
    boost::asio::streambuf buffer_;

    std::vector<std::pair<std::string,std::string>> request_headers_;
    std::string request_method_, request_uri_;
    Server &server_;
};

struct Server {
    std::vector<Service> services;

    Server(boost::asio::io_service& io_service, unsigned short port)
        : acceptor_(io_service, boost::asio::ip::tcp::endpoint(boost::asio::ip::tcp::v4(), port)),
              socket_(io_service)
    {
        do_accept();
    }

    void operator += (Service &&callable) {
        services.push_back(std::move(callable));
    }

    std::string operator()(Request &&req) {
        for (Service &service : services) {
            if (service.signature_matches(req.uri_parts)) {
                return service(std::move(req));
            }
        }
        throw "batat";
    }
private:

    void do_accept()
    {
        acceptor_.async_accept(socket_, [this](boost::system::error_code ec)
        {
            if (!ec)
            {
                std::make_shared<session>(std::move(socket_), *this)->start();
            }

            do_accept();
        });
    }

    boost::asio::ip::tcp::acceptor acceptor_;
    boost::asio::ip::tcp::socket socket_;
};



std::string server_handle(Server &server, Request &&request)
{
    return server(std::move(request));
}

int main()
{
    boost::asio::io_service io_service;
    Server server(io_service, 8080);

    server += get<int>() / "math" / Param<int>("value1") / "plus" / Param<int>("value2") = [](int a, int b) {
        return a+b;
    };

    server += get<int>() / "math" / Param<int>("value1") / "minus" / Param<int>("value2") = [](int a, int b) {
        return a-b;
    };

    server += get<int>() / "math" / Param<int>("value1") / "times" / Param<int>("value2") = [](int a, int b) {
        return a*b;
    };

    server += get<int>() / "string" / "length" / Param<std::string>("value") = [](std::string const &s) {
        return static_cast<int>( s.size() );
    };

    server += get<std::string>() / "string" / "reverse" / Param<std::string>("value") = [](std::string s) {
        std::reverse(s.begin(), s.end());
        return s;
    };

    server += get<std::string>() / "header" / Header<std::string>("Accept") = [](std::string s) {

        return s;
    };

    io_service.run();

    return 0;
}
