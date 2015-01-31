#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <functional>
#include <algorithm>
#include <memory>

template<typename T>
struct Arg {
    const char *name;

    Arg(const char *name) : name(name)  {}
};

struct PathPart {
    enum Type { Const, Var };
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

class Service {
public:
    template<typename Ret, typename ...Args>
    Service(std::vector<PathPart> &&sig, std::function<Ret(Args...)> &&func) {
        m_impl.reset(new OpsImpl<Ret,Args...>(std::move(sig), std::move(func)));
    }

    Service(Service &&that)
        : m_impl(std::move(that.m_impl)) {}

    std::string operator()(const std::vector<std::string> &uri) {
        return m_impl->call(uri);
    }

    const std::vector<PathPart> &signature() const { return m_impl->signature; }

    bool signature_matches(const std::vector<std::string> &uri) {
        if (uri.size() != signature().size()) {
            return false;
        }
        for (unsigned i=0; i<uri.size(); ++i) {
            const PathPart &sig_part = signature()[i];
            if (sig_part.type == PathPart::Const && sig_part.value != uri[i]) {
                return false;
            }
        }
        return true;
    }

private:
    struct Ops {
        std::vector<PathPart> signature;
        virtual std::string call(const std::vector<std::string> &) = 0;
    };
    std::unique_ptr<Ops> m_impl;

    template<typename Ret, typename ...Args>
    struct OpsImpl : Ops {
        std::function<Ret(Args...)> func;
        OpsImpl(std::vector<PathPart> &&sig, std::function<Ret(Args...)> &&func)
            : func(std::move(func)) {
            signature = std::move(sig);
        }

        std::string call(const std::vector<std::string> &args) override {
            std::vector<const std::string*> uri_args;
            for (unsigned i=0; i<signature.size(); ++i) {
                if (signature[i].type == PathPart::Var) {
                    uri_args.push_back(&args[i]);
                }
            }
            return to_string_helper( call_func( uri_args, typename gen_seq<sizeof...(Args)>::type() ) );
        }

        template<int ...S>
        Ret call_func(const std::vector<const std::string*> &uri_args, seq<S...>)
        {
            return func( from_string_helper<Args>( *uri_args[S] )... );
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
ServiceBuilder<Ret,Args..., T> operator/(ServiceBuilder<Ret,Args...> &&s, const Arg<T> &&arg) {
    ServiceBuilder<Ret,Args...,T> result { std::move(s.signature) };
    result.signature.push_back({PathPart::Var, arg.name});
    return result;
}

template<typename Ret>
ServiceBuilder<Ret> get() {
    return ServiceBuilder<Ret>{ { { PathPart::Const, "GET" } } };
}


#include <boost/asio.hpp>
#include <boost/algorithm/string/trim.hpp>

struct Server;
std::string server_handle(Server &server, const std::string &uri);

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
                        const std::string resp = server_handle(server_, request_method_ + request_uri_);
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

    std::string operator()(const std::string &uri) {
        std::vector<std::string> args = parse(uri);
        for (Service &service : services) {
            if (service.signature_matches(args)) {
                return service(args);
            }
        }
        throw "batat";
    }
private:
    std::vector<std::string> parse(const std::string &uri) {
        std::vector<std::string> args;
        std::stringstream ss(uri);
        std::string item;

        while (std::getline(ss, item, '/')) {
            args.push_back(item);
        }
        return args;
    }

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

std::string server_handle(Server &server, const std::string &uri)
{
    return server(uri);
}

int main()
{
    boost::asio::io_service io_service;
    Server server(io_service, 8080);

    server += get<int>() / "math" / Arg<int>("value1") / "plus" / Arg<int>("value2") = [](int a, int b) {
        return a+b;
    };

    server += get<int>() / "math" / Arg<int>("value1") / "minus" / Arg<int>("value2") = [](int a, int b) {
        return a-b;
    };

    server += get<int>() / "math" / Arg<int>("value1") / "times" / Arg<int>("value2") = [](int a, int b) {
        return a*b;
    };

    server += get<int>() / "string" / "length" / Arg<std::string>("value") = [](std::string const &s) {
        return s.size();
    };

    server += get<std::string>() / "string" / "reverse" / Arg<std::string>("value") = [](std::string s) {
        std::reverse(s.begin(), s.end());
        return s;
    };

    io_service.run();

    return 0;
}
