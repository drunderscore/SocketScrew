#include <LibCore/EventLoop.h>
#include <LibCore/UDPServer.h>
#include <LibCore/UDPSocket.h>
#include <LibCore/ArgsParser.h>
#include <LibCore/Timer.h>
#include <time.h>
#include <signal.h>
#include <AK/Random.h>
#include <AK/NumberFormat.h>

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
static double s_chance_to_duplicate_packet_clientbound = 0.0;
static double s_chance_to_duplicate_packet_serverbound = 0.0;
static bool s_fake_clientbound_jitter = false;
static bool s_fake_serverbound_jitter = false;
static bool s_small_modification = false;
// TODO: This vector is never emptied
static Vector<AK::NonnullRefPtr<Core::Timer>, 256> m_latent_packets;
static Vector<int, 1024> s_clientbound_latency_history;
static Vector<int, 1024> s_serverbound_latency_history;
static Optional<sockaddr_in> who = {};
static constexpr int who_len = sizeof(sockaddr_in);

// Statistics
static u64 s_total_clientbound_packets = 0;
static u64 s_total_serverbound_packets = 0;
static u64 s_total_clientbound_bytes = 0;
static u64 s_total_serverbound_bytes = 0;
static u64 s_total_clientbound_dropped_packets = 0;
static u64 s_total_serverbound_dropped_packets = 0;
static u64 s_total_clientbound_dropped_bytes = 0;
static u64 s_total_serverbound_dropped_bytes = 0;
static u64 s_total_clientbound_modified_packets = 0;
static u64 s_total_serverbound_modified_packets = 0;
static u64 s_total_clientbound_duplicated_packets = 0;
static u64 s_total_serverbound_duplicated_packets = 0;

void send_to_client(NonnullRefPtr<Core::UDPSocket> the, NonnullRefPtr<Core::UDPServer> our, ByteBuffer bytes)
{
    auto who_ptr = *who;
    sendto(our->fd(), bytes.data(), bytes.size(), 0, (struct sockaddr*) &who_ptr, who_len);
}

void forward_to_client(NonnullRefPtr<Core::UDPSocket> the, NonnullRefPtr<Core::UDPServer> our, ByteBuffer bytes)
{
    if (s_fake_clientbound_latency > 0)
    {
        auto delay = s_fake_clientbound_jitter ? AK::get_random_uniform(s_fake_clientbound_latency)
                                               : s_fake_clientbound_latency;
        s_clientbound_latency_history.append(delay);
        auto timer = Core::Timer::create_single_shot(delay, [=, bytes = move(bytes)]() mutable
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
        auto delay = s_fake_serverbound_jitter ? AK::get_random_uniform(s_fake_serverbound_latency)
                                               : s_fake_serverbound_latency;
        s_serverbound_latency_history.append(delay);
        auto timer = Core::Timer::create_single_shot(delay, [=, bytes = move(bytes)]() mutable
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

void on_sigint(int)
{
    outln();

    if (s_fake_clientbound_latency > 0)
    {
        outln("Fake Clientbound Latency: {}ms, jitter: {}", s_fake_clientbound_latency, s_fake_clientbound_jitter);
        auto latency_size = s_clientbound_latency_history.size();

        if (latency_size > 0)
        {
            u64 total_latency = 0;
            for (auto& val : s_clientbound_latency_history)
                total_latency += val;
            outln("Average Clientbound Latency: {:.2}ms",
                  (double) total_latency / (double) latency_size);
        }
    }

    if (s_fake_serverbound_latency > 0)
    {
        outln("Fake Serverbound Latency: {}ms, jitter: {}", s_fake_serverbound_latency, s_fake_serverbound_jitter);
        auto latency_size = s_serverbound_latency_history.size();

        if (latency_size > 0)
        {
            u64 total_latency = 0;
            for (auto& val : s_serverbound_latency_history)
                total_latency += val;
            outln("Average Serverbound Latency: {:.2}ms",
                  (double) total_latency / (double) latency_size);
        }
    }

    outln("Total Clientbound Packets: {}", s_total_clientbound_packets);
    outln("Total Serverbound Packets: {}", s_total_serverbound_packets);

    outln("Total Clientbound Bytes: {}", AK::human_readable_size_long(s_total_clientbound_bytes));
    outln("Total Serverbound Bytes: {}", AK::human_readable_size_long(s_total_serverbound_bytes));

    if (s_chance_to_drop_clientbound > 0.0 && s_total_clientbound_packets != 0)
    {
        outln("Total Clientbound Dropped Packets: {} ({:.2}% of total packets)", s_total_clientbound_dropped_packets,
              ((double) s_total_clientbound_dropped_packets / (double) s_total_clientbound_packets) * 100);
        outln("Total Clientbound Dropped Bytes: {} ({:.2}% of total bytes)",
              AK::human_readable_size_long(s_total_clientbound_dropped_bytes),
              ((double) s_total_clientbound_dropped_bytes / (double) s_total_clientbound_bytes) * 100);
    }

    if (s_chance_to_drop_serverbound > 0.0 && s_total_serverbound_packets != 0)
    {
        outln("Total Serverbound Dropped Packets: {} ({:.2}% of total packets)",
              s_total_serverbound_dropped_packets,
              ((double) s_total_serverbound_dropped_packets / (double) s_total_serverbound_packets) * 100);
        outln("Total Serverbound Dropped Bytes: {} ({:.2}% of total bytes)",
              AK::human_readable_size_long(s_total_serverbound_dropped_bytes),
              ((double) s_total_serverbound_dropped_bytes / (double) s_total_serverbound_bytes) * 100);
    }

    if (s_chance_to_modify_clientbound > 0.0 && s_total_clientbound_packets != 0)
    {
        outln("Total Clientbound Modified Packets: {} ({:.2}% of total packets)", s_total_clientbound_modified_packets,
              ((double) s_total_clientbound_modified_packets / (double) s_total_clientbound_packets) * 100);
    }

    if (s_chance_to_modify_serverbound > 0.0 && s_total_serverbound_packets != 0)
    {
        outln("Total Serverbound Modified Packets: {} ({:.2}% of total packets)", s_total_serverbound_modified_packets,
              ((double) s_total_serverbound_modified_packets / (double) s_total_serverbound_packets) * 100);
    }

    if (s_chance_to_duplicate_packet_clientbound > 0.0 && s_total_clientbound_packets != 0)
    {
        outln("Total Clientbound Duplicated Packets: {} ({:.2}% of total packets)",
              s_total_clientbound_duplicated_packets,
              ((double) s_total_clientbound_duplicated_packets / (double) s_total_clientbound_packets) * 100);
    }

    if (s_chance_to_duplicate_packet_serverbound > 0.0 && s_total_serverbound_packets != 0)
    {
        outln("Total Serverbound Duplicated Packets: {} ({:.2}% of total packets)",
              s_total_serverbound_duplicated_packets,
              ((double) s_total_serverbound_duplicated_packets / (double) s_total_serverbound_packets) * 100);
    }

    exit(0);
}

int main(int argc, char** argv)
{
    struct sigaction sa = {};
    sa.sa_handler = on_sigint;
    sigaction(SIGINT, &sa, nullptr);

    srand(time(nullptr));
    Core::ArgsParser args_parser;

    static int the_server_port;
    static int our_server_port;
    static String the_server_address_string;
    static IPv4Address the_server_address;

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
    args_parser.add_option(s_fake_serverbound_jitter,
                           "Makes the server-latency option random (from 0 to server-latency ms), simulating packet jitter.",
                           "server-jitter", 'j');
    args_parser.add_option(s_fake_clientbound_jitter,
                           "Makes the client-latency option random (from 0 to client-latency ms), simulating packet jitter.",
                           "client-jitter", 'J');
    args_parser.add_option(s_chance_to_duplicate_packet_serverbound,
                           "Chance to duplicate a serverbound packet.",
                           "server-duplicate", 'u', "Chance");
    args_parser.add_option(s_chance_to_duplicate_packet_clientbound,
                           "Chance to duplicate a clientbound packet.",
                           "client-duplicate", 'U', "Chance");
    args_parser.add_positional_argument(the_server_address_string, "Address of the server to forward", "The address");
    args_parser.add_positional_argument(the_server_port, "Port of the server to forward", "The port");
    args_parser.add_positional_argument(our_server_port,
                                        "Port of the server to bind to, that will forward packets to The address",
                                        "The port");
    if (!args_parser.parse(argc, argv))
        return 1;

    auto maybe_the_server_address = IPv4Address::from_string(the_server_address_string);
    if (!maybe_the_server_address.has_value())
    {
        warnln("Invalid address.");
        return 2;
    }

    the_server_address = maybe_the_server_address.release_value();

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
        s_total_clientbound_packets++;
        s_total_clientbound_bytes += bytes.size();
        if (s_chance_to_drop_clientbound > (double) (rand() / (double) RAND_MAX))
        {
            dbgln_if(DROP_PACKET_DEBUG, "Dropping clientbound packet");
            s_total_clientbound_dropped_packets++;
            s_total_clientbound_dropped_bytes += bytes.size();
            return;
        }

        if (s_chance_to_modify_clientbound > (double) (rand() / (double) RAND_MAX))
        {
            s_total_clientbound_modified_packets++;
            auto random_index = AK::get_random_uniform(bytes.size());
            if (s_small_modification)
                bytes[random_index]--;
            else
                bytes[random_index] = AK::get_random<u8>();

            dbgln_if(MODIFY_PACKET_DEBUG, "Modifying clientbound packet");
        }

        dbgln_if(FORWARD_SIZE_DEBUG, "{} bytes to client", bytes.size());
        if (s_chance_to_duplicate_packet_clientbound > (double) (rand() / (double) RAND_MAX))
        {
            s_total_clientbound_duplicated_packets++;
            auto bytes_copy = bytes;
            forward_to_client(the_server, our_server, move(bytes_copy));
        }
        forward_to_client(the_server, our_server, move(bytes));
    };

    our_server->on_ready_to_receive = [=]() mutable
    {
        auto bytes = our_server->receive(max_data_length, last_who);
        if (!who.has_value())
        {
            who = last_who;
            outln("Someone has connected, using them");
        }

        s_total_serverbound_packets++;
        s_total_serverbound_bytes += bytes.size();

        if (s_chance_to_drop_serverbound > (double) (rand() / (double) RAND_MAX))
        {
            dbgln_if(DROP_PACKET_DEBUG, "Dropping serverbound packet");
            s_total_serverbound_dropped_packets++;
            s_total_serverbound_dropped_bytes += bytes.size();
            return;
        }

        if (s_chance_to_modify_serverbound > (double) (rand() / (double) RAND_MAX))
        {
            s_total_serverbound_modified_packets++;
            auto random_index = AK::get_random_uniform(bytes.size());
            if (s_small_modification)
                bytes[random_index]--;
            else
                bytes[random_index] = AK::get_random<u8>();
            dbgln_if(MODIFY_PACKET_DEBUG, "Modifying serverbound packet");
        }

        dbgln_if(FORWARD_SIZE_DEBUG, "{} bytes to server", bytes.size());
        if (s_chance_to_duplicate_packet_serverbound > (double) (rand() / (double) RAND_MAX))
        {
            s_total_serverbound_duplicated_packets++;
            auto bytes_copy = bytes;
            forward_to_server(the_server, our_server, move(bytes_copy));
        }
        forward_to_server(the_server, our_server, move(bytes));
    };

    the_server->on_connected = [&]
    {
        outln("Connected to the server.");
    };

    our_server->bind({}, our_server_port);
    the_server->connect(the_server_address, the_server_port);

    return event_loop.exec();
}