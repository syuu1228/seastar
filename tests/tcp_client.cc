/*
 * Copyright (C) 2014 Cloudius Systems, Ltd.
 */

#include "core/app-template.hh"
#include "core/future-util.hh"

using namespace net;
using namespace std::chrono_literals;

static int tx_msg_total_size = 100 * 1024 * 1024;
static int tx_msg_size = 4 * 1024;
static int tx_msg_nr = tx_msg_total_size / tx_msg_size;
static std::string str_txbuf(tx_msg_size, 'X');

class client {
private:
    std::vector<connected_socket> _sockets;
public:
    class connection {
        connected_socket _fd;
        input_stream<char> _read_buf;
        output_stream<char> _write_buf;
    public:
        connection(connected_socket&& fd)
            : _fd(std::move(fd))
            , _read_buf(_fd.input())
            , _write_buf(_fd.output()) {}
        future<> do_write(int end) {
            if (end == 0) {
                return make_ready_future<>();
            }
            return _write_buf.write(str_txbuf).then([this, end] {
                return _write_buf.flush();
            }).then([this, end] {
                return do_write(end - 1);
            });
        }
        future<> tx_test() {
            return do_write(tx_msg_nr).then([this] {
                return _write_buf.close();
            }).then([this] {
                return make_ready_future<>();
            });
        }
    };

    void start(ipv4_addr server_addr) {
        auto fd = engine.net().connect(make_ipv4_address(server_addr));
            _sockets.push_back(std::move(fd));
            auto conn = new connection(std::move(_sockets[0]));
            conn->tx_test().rescue([this, conn] (auto get_ex) {
                delete conn;
                try {
                    get_ex();
                } catch (std::exception& ex) {
                    std::cout << "request error " << ex.what() << "\n";
                }
            });
    }
};

namespace bpo = boost::program_options;

int main(int ac, char ** av) {
    client _client;
    app_template app;
    app.add_options()
        ("server", bpo::value<std::string>(), "Server address")
        ;
    return app.run(ac, av, [&_client, &app] {
        auto&& config = app.configuration();
        _client.start(config["server"].as<std::string>());
    });
}
