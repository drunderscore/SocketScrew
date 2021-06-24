#include <LibCore/EventLoop.h>
#include <LibCore/UDPServer.h>
#include <LibCore/UDPSocket.h>
#include <LibCore/ArgsParser.h>
#include <LibCore/Timer.h>
#include <time.h>
#include <AK/Random.h>

#define FORWARD_SIZE_DEBUG 0
#define DROP_PACKET_DEBUG 0
#define MODIFY_PACKET_DEBUG 0

static constexpr size_t max_data_length = KiB * 16;

static int s_fake_clientbound_latency = 0;
static int s_fake_serverbound_latency = 0;
static double s_chance_to_drop_clientbound = 0.0;
static double s_chance_to_drop_serverbound = 0.0;
static double s_chance_to_modify_clientbound = 0.0;
static double s_chance_to_modify_serverbound = 0.0;
static bool s_small_modification = false;
// TODO: This vector is never emptied
static Vector<AK::NonnullRefPtr<Core::Timer>, 256> m_latent_packets;
static Optional<sockaddr_in> who = {};
static constexpr int who_len = sizeof(sockaddr_in);

void send_to_client(NonnullRefPtr<Core::UDPSocket> the, NonnullRefPtr<Core::UDPServer> our, ByteBuffer bytes)
{
    auto who_ptr = *who;
    sendto(our->fd(), bytes.data(), bytes.size(), 0, (struct sockaddr*) &who_ptr, who_len);
}

void forward_to_client(NonnullRefPtr<Core::UDPSocket> the, NonnullRefPtr<Core::UDPServer> our, ByteBuffer bytes)
{
    if (s_fake_clientbound_latency > 0)
    {
        auto timer = Core::Timer::create_single_shot(s_fake_clientbound_latency, [=, bytes = move(bytes)]() mutable
        {
            send_to_client(the, our, move(bytes));
        });
        timer->start();
        m_latent_packets.append(move(timer));
    }
    else
    {
        send_to_client(the, our, move(bytes));
    }
}

void send_to_server(NonnullRefPtr<Core::UDPSocket> the, NonnullRefPtr<Core::UDPServer> our, ByteBuffer bytes)
{
    the->send(bytes);
}

void forward_to_server(NonnullRefPtr<Core::UDPSocket> the, NonnullRefPtr<Core::UDPServer> our, ByteBuffer bytes)
{
    if (s_fake_serverbound_latency > 0)
    {
        auto timer = Core::Timer::create_single_shot(s_fake_serverbound_latency, [=, bytes = move(bytes)]() mutable
        {
            send_to_server(the, our, move(bytes));
        });
        timer->start();
        m_latent_packets.append(move(timer));
    }
    else
    {
        send_to_server(the, our, move(bytes));
    }
}

int main(int argc, char** argv)
{
    srand(time(nullptr));
    Core::ArgsParser args_parser;

    static int the_server_port;
    static int our_server_port;

    args_parser.add_option(the_server_port, "The server port that clients get forwarded to.", "the-port", 'P',
                           "Port");
    args_parser.add_option(our_server_port, "The server port that clients connect to get to us.", "our-port", 'p',
                           "Port");
    args_parser.add_option(s_fake_serverbound_latency,
                           "Introduce a constant fake serverbound latency to packets, in milliseconds",
                           "server-latency", 'l', "Latency");
    args_parser.add_option(s_fake_clientbound_latency,
                           "Introduce a constant fake clientbound latency to packets, in milliseconds",
                           "client-latency", 'L', "Latency");
    args_parser.add_option(s_chance_to_drop_serverbound,
                           "Chance to drop a serverbound packet, from 0.0 to 1.0",
                           "server-drop", 'd', "Chance");
    args_parser.add_option(s_chance_to_drop_clientbound,
                           "Chance to drop a clientbound packet, from 0.0 to 1.0",
                           "client-drop", 'D', "Chance");
    args_parser.add_option(s_chance_to_modify_serverbound,
                           "Chance to modify a serverbound packet, from 0.0 to 1.0",
                           "server-modify", 'm', "Chance");
    args_parser.add_option(s_chance_to_modify_clientbound,
                           "Chance to modify a clientbound packet, from 0.0 to 1.0",
                           "client-modify", 'M', "Chance");
    args_parser.add_option(s_small_modification,
                           "Only increment or decrement that single byte",
                           "small", 's');
    if (!args_parser.parse(argc, argv))
        return 1;

    Core::EventLoop event_loop;

    auto our_server = Core::UDPServer::construct();
    auto the_server = Core::UDPSocket::construct();
    sockaddr_in last_who = {};

    the_server->on_ready_to_read = [=]() mutable
    {
        if (!who.has_value())
        {
            warnln("We don't know who this is yet, cannot forward to client.");
            return;
        }

        auto bytes = the_server->receive(max_data_length);
        if (s_chance_to_drop_clientbound > (double) (rand() / (double) RAND_MAX))
        {
            dbgln_if(DROP_PACKET_DEBUG, "Dropping clientbound packet");
            return;
        }
        if (s_chance_to_modify_clientbound > (double) (rand() / (double) RAND_MAX))
        {
            auto random_index = AK::get_random_uniform(bytes.size());
            if (s_small_modification)
                bytes[random_index]--;
            else
                bytes[random_index] = AK::get_random<u8>();

            dbgln_if(MODIFY_PACKET_DEBUG, "Modifying clientbound packet");
        }
        dbgln_if(FORWARD_SIZE_DEBUG, "{} bytes to client", bytes.size());
        forward_to_client(the_server, our_server, move(bytes));
    };

    our_server->on_ready_to_receive = [=]() mutable
    {
        auto bytes = our_server->receive(max_data_length, last_who);
        if (!who.has_value())
        {
            who = last_who;
            outln("Someone has connected, using them ");
        }
        if (s_chance_to_drop_serverbound > (double) (rand() / (double) RAND_MAX))
        {
            dbgln_if(DROP_PACKET_DEBUG, "Dropping serverbound packet");
            return;
        }
        if (s_chance_to_modify_serverbound > (double) (rand() / (double) RAND_MAX))
        {
            auto random_index = AK::get_random_uniform(bytes.size());
            if (s_small_modification)
                bytes[random_index]--;
            else
                bytes[random_index] = AK::get_random<u8>();
            dbgln_if(MODIFY_PACKET_DEBUG, "Modifying serverbound packet");
        }
        dbgln_if(FORWARD_SIZE_DEBUG, "{} bytes to server", bytes.size());
        forward_to_server(the_server, our_server, move(bytes));
    };

    the_server->on_connected = [&]
    {
        outln("Connected to the server.");
    };

    our_server->bind({}, our_server_port);
    the_server->connect(Core::SocketAddress({10, 0, 0, 20}), the_server_port);

    return event_loop.exec();
}