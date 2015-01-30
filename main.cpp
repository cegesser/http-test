#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <functional>
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
    return ServiceBuilder<Ret>();
}


struct Server {
    std::vector<Service> services;

    Server(const std::string &host, unsigned short port) {
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
        std::getline(ss, item, '/');//skips first '/'
        while (std::getline(ss, item, '/')) {
            args.push_back(item);
        }
        return args;
    }
};

int main()
{
    Server server("localhost", 8080);

    server += get<int>() / "math" / Arg<int>("value1") / "plus" / Arg<int>("value2") = [](int a, int b) {
        return a+b;
    };

    server += get<int>() / "math" / Arg<int>("value1") / "minus" / Arg<int>("value2") = [](int a, int b) {
        return a-b;
    };

    server += get<int>() / "math" / Arg<int>("value1") / "times" / Arg<int>("value2") = [](int a, int b) {
        return a*b;
    };

    server += get<std::string>() / "util" / "to-string" / Arg<int>("value") = [](int i) {
        return std::to_string(i);
    };


    std::cout << server( "/math/4/times/a5" )   << std::endl;
    std::cout << server( "/math/10/minus/2" )  << std::endl;
    std::cout << server( "/math/3/plus/4" )    << std::endl;
    std::cout << server( "/util/to-string/4" ) << std::endl;

    return 0;
}
