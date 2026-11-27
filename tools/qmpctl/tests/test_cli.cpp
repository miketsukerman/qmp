// SPDX-License-Identifier: Apache-2.0
// qmpctl commands, run in-process against real servers: on the host over the UDS
// transport (a private socket directory per test process), on QNX over native message
// passing. Set QMPCTL_TEST_TRANSPORT to force one. The servers use generated bindings;
// qmpctl itself sees only what they serve as reflection data.
#include <gtest/gtest.h>

#include <qmp/qmp_client.h>
#include <qmp/qmp_server.h>
#include <qmp/qmp_transport.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <dirent.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "kitchen.qmp.h"
#include "qmpctl.hpp"
#include "reflect.hpp"

namespace {

using namespace std::chrono_literals;
using qmpctl::Json;

std::string g_dir; // private UDS socket directory, also used for scratch files

void remove_dir() {
    if (DIR *d = opendir(g_dir.c_str())) {
        while (const dirent *e = readdir(d)) {
            if (e->d_name[0] != '.') {
                (void)unlink((g_dir + "/" + e->d_name).c_str());
            }
        }
        (void)closedir(d);
    }
    (void)rmdir(g_dir.c_str());
}

const std::string &transport_name() {
    static const std::string name = [] {
        const char *env = std::getenv("QMPCTL_TEST_TRANSPORT");
        std::string n = env != nullptr ? env : qmp_transport_default()->name;
        char tmpl[] = "/tmp/qmpctl-test-XXXXXX";
        if (mkdtemp(tmpl) != nullptr) {
            g_dir = tmpl;
            (void)setenv("QMP_UDS_DIR", g_dir.c_str(), 1);
            (void)atexit(remove_dir);
        }
        return n;
    }();
    return name;
}

struct Result {
    int code = -1;
    std::string out;
    std::string err;
};

Result ctl(std::vector<std::string> args, const std::string &input = "") {
    args.insert(args.begin(), {"--transport", transport_name()});
    std::istringstream in(input);
    std::ostringstream out;
    std::ostringstream err;
    Result r;
    r.code = qmpctl::run(args, in, out, err);
    r.out = out.str();
    r.err = err.str();
    return r;
}

bool contains(const std::string &hay, const std::string &needle) {
    return hay.find(needle) != std::string::npos;
}

std::vector<std::string> lines(const std::string &text) {
    std::vector<std::string> out;
    std::istringstream ss(text);
    for (std::string l; std::getline(ss, l);) {
        out.push_back(l);
    }
    return out;
}

// ---- the Kitchen service (generated bindings) ---------------------------------------------

struct Kitchen {
    qmp_server_t server{};
    std::vector<uint64_t> workbuf;
    std::vector<uint64_t> topicbuf;
    std::thread runner;
    std::mutex mu;
    std::vector<int32_t> fired; // payloads of the one-way Fire
    std::atomic<int> pings{0};
    std::atomic<int> echoes{0};
    qmp_service_desc_t desc = qmp_test_kitchen_v1_Kitchen_desc;
    std::vector<qmp_method_desc_t> methods{desc.methods, desc.methods + desc.nmethods};
    qmp_test_kitchen_v1_Kitchen_ops ops{};

    static int echo(void *ctx, qmp_req_t *, const qmp_test_kitchen_v1_Sink *in,
                    qmp_test_kitchen_v1_Sink *out, int32_t *) {
        static_cast<Kitchen *>(ctx)->echoes++;
        *out = *in;
        return QMP_HANDLED;
    }
    static int fail(void *, qmp_req_t *, const qmp_test_kitchen_v1_Inner *in,
                    qmp_test_kitchen_v1_Inner *out, int32_t *status) {
        if (in->a < 0) {
            return EXDEV; // transport-level error
        }
        *status = in->a;
        *out = *in;
        return QMP_HANDLED;
    }
    static int ping(void *ctx, qmp_req_t *, int32_t *) {
        static_cast<Kitchen *>(ctx)->pings++;
        return QMP_HANDLED;
    }
    // protoc-gen-qmp does not emit one-way trampolines yet (the generated descriptor
    // leaves the handler NULL, so the call would fail with ENOSYS). The runtime supports
    // them, so the test installs its own to exercise qmpctl's one-way path end to end.
    static int fire(const void *, void *ctx, qmp_req_t *, const void *in, void *, int32_t *) {
        auto *k = static_cast<Kitchen *>(ctx);
        std::lock_guard<std::mutex> lk(k->mu);
        k->fired.push_back(static_cast<const qmp_test_kitchen_v1_Inner *>(in)->a);
        return QMP_HANDLED;
    }
    static int publish(void *ctx, qmp_req_t *, const qmp_test_kitchen_v1_Inner *in, int32_t *) {
        auto *k = static_cast<Kitchen *>(ctx);
        if (qmp_test_kitchen_v1_Kitchen_publish_Tick(&k->server, in) != EOK) {
            return EIO;
        }
        for (int32_t i = 0; i < in->a; i++) {
            qmp_test_kitchen_v1_Sink s;
            std::memset(&s, 0, sizeof(s));
            s.i32 = i;
            if (qmp_test_kitchen_v1_Kitchen_publish_Log(&k->server, &s) != EOK) {
                return EIO;
            }
        }
        return QMP_HANDLED;
    }

    // reflection = false builds a service that cannot describe itself (hand-written
    // descriptor, or a runtime older than QMP_OP_REFLECT).
    int start(uint16_t instance = 0, bool reflection = true, const char *transport = nullptr) {
        if (!reflection) {
            desc.reflect_json = nullptr;
            desc.reflect_json_len = 0;
        }
        ops.Echo = echo;
        ops.Fail = fail;
        ops.Ping = ping;
        for (qmp_method_desc_t &m : methods) {
            if (m.id == QMP_TEST_KITCHEN_V1_KITCHEN_M_FIRE) {
                m.handler = fire;
            }
        }
        desc.methods = methods.data();
        ops.Publish = publish;
        constexpr int kThreads = 2;
        workbuf.assign(
            QMP_SERVER_WORKBUF_SIZE(QMP_TEST_KITCHEN_V1_KITCHEN_MAX_MSG_SIZE, kThreads) / 8 + 1, 0);
        topicbuf.assign(QMP_TEST_KITCHEN_V1_KITCHEN_TOPIC_STORAGE_SIZE / 8 + 1, 0);
        qmp_server_cfg_t cfg{};
        cfg.transport =
            qmp_transport_by_name(transport != nullptr ? transport : transport_name().c_str());
        cfg.nthreads = kThreads;
        cfg.workbuf = workbuf.data();
        cfg.workbuf_size = workbuf.size() * 8;
        cfg.topicbuf = topicbuf.data();
        cfg.topicbuf_size = topicbuf.size() * 8;
        int rc = qmp_server_init(&server, &cfg);
        if (rc == EOK) {
            rc = qmp_server_register(&server, &desc, instance, &ops, this);
        }
        if (rc == EOK) {
            runner = std::thread([this] { (void)qmp_server_run(&server); });
        }
        return rc;
    }

    void stop() {
        if (runner.joinable()) {
            (void)qmp_server_stop(&server);
            runner.join();
            (void)qmp_server_close(&server);
        }
    }
    ~Kitchen() { stop(); }
};

class Cli : public ::testing::Test {
  protected:
    Kitchen kitchen_;
    void SetUp() override {
        ASSERT_NE(qmp_transport_by_name(transport_name().c_str()), nullptr)
            << "transport " << transport_name() << " is not built in";
        ASSERT_EQ(kitchen_.start(), EOK);
    }
};

// ---- list ---------------------------------------------------------------------------------

TEST_F(Cli, ListShowsWhatEachServiceSaysAboutItself) {
    Kitchen second;
    ASSERT_EQ(second.start(1), EOK);
    Kitchen mute;
    ASSERT_EQ(mute.start(2, false), EOK);

    Result r = ctl({"list"});
    ASSERT_EQ(r.code, 0) << r.err;
    const std::vector<std::string> rows = lines(r.out);
    ASSERT_GE(rows.size(), 4u) << r.out;
    EXPECT_TRUE(contains(rows[0], "NAME")) << rows[0];
    auto row = [&](const std::string &name) {
        for (const std::string &l : rows) {
            if (l.rfind(name, 0) == 0) {
                return l;
            }
        }
        return std::string();
    };
    EXPECT_TRUE(contains(row("qmp/0777-0000/v2"), "qmp.test.Kitchen  2.3      5        2       /"))
        << r.out;
    EXPECT_TRUE(contains(row("qmp/0777-0001/v2"), "qmp.test.Kitchen")) << r.out;
    EXPECT_TRUE(contains(row("qmp/0777-0002/v2"), "(no reflection data")) << r.out;

    r = ctl({"list", "--json"});
    ASSERT_EQ(r.code, 0) << r.err;
    const Json j = Json::parse(r.out);
    ASSERT_TRUE(j.is_array());
    int seen = 0;
    for (const Json &e : j) {
        if (e["service_id"] != 0x0777) {
            continue; // on a shared target other services may be registered
        }
        seen++;
        EXPECT_EQ(e["reachable"], true);
        EXPECT_EQ(e["version"]["major"], 2);
        EXPECT_EQ(e["version"]["minor"], 3);
        if (e["instance_id"] == 2) {
            EXPECT_TRUE(e.contains("error"));
        } else {
            EXPECT_EQ(e["service"], "qmp.test.Kitchen");
            EXPECT_EQ(e["methods"], 5);
            EXPECT_EQ(e["topics"], 2);
        }
    }
    EXPECT_EQ(seen, 3);
    EXPECT_EQ(ctl({"list", "extra"}).code, 2);
}

TEST_F(Cli, ListFlagsAStaleSocketOfAKilledServer) {
    if (transport_name() != "uds") {
        GTEST_SKIP() << "socket files exist only on the uds transport";
    }
    // What kill -9 leaves behind: a bound socket file nobody listens on.
    sockaddr_un sa = {};
    sa.sun_family = AF_UNIX;
    (void)std::snprintf(sa.sun_path, sizeof(sa.sun_path), "%s/qmp.0999-0000.v1.sock",
                        g_dir.c_str());
    int fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    ASSERT_GE(fd, 0);
    ASSERT_EQ(bind(fd, reinterpret_cast<struct sockaddr *>(&sa), sizeof(sa)), 0);
    (void)close(fd);
    // Files that only look similar are not services.
    std::ofstream(g_dir + "/qmp.09zz-0000.v1.sock") << "x";
    std::ofstream(g_dir + "/qmp.0999-0000.v1.sock.bak") << "x";

    Result r = ctl({"list"});
    ASSERT_EQ(r.code, 0) << r.err;
    EXPECT_TRUE(contains(r.out, "qmp/0999-0000/v1")) << r.out;
    EXPECT_TRUE(contains(r.out, "(not running (stale name))")) << r.out;
    EXPECT_FALSE(contains(r.out, "09zz")) << r.out;
    EXPECT_EQ(lines(r.out).size(), 3u) << r.out;
    (void)unlink(sa.sun_path);
}

// ---- describe -----------------------------------------------------------------------------

TEST_F(Cli, DescribeServiceByNameAndById) {
    Result r = ctl({"describe", "Kitchen"});
    ASSERT_EQ(r.code, 0) << r.err;
    EXPECT_TRUE(contains(r.out, "service qmp.test.kitchen.v1.Kitchen")) << r.out;
    EXPECT_TRUE(contains(r.out, "id        0x0777  instance 0x0000"));
    EXPECT_TRUE(contains(r.out, "version   2.3"));
    EXPECT_TRUE(contains(r.out, "endpoint  qmp/0777-0000/v2"));
    EXPECT_TRUE(contains(r.out, std::string("runtime   ") + QMP_VERSION_STRING + "  (wire " +
                                    std::to_string(QMP_WIRE_VERSION) + ")\n"))
        << r.out;
    EXPECT_TRUE(contains(r.out, "0x0001  Echo(qmp.test.kitchen.v1.Sink) returns "
                                "(qmp.test.kitchen.v1.Sink)"));
    EXPECT_TRUE(contains(r.out, "0x0004  Fire(qmp.test.kitchen.v1.Inner) returns "
                                "(qmp.test.kitchen.v1.Empty)  [oneway]"));
    EXPECT_TRUE(contains(r.out, "0x8001  Tick  qmp.test.kitchen.v1.Inner  LATEST"));
    EXPECT_TRUE(contains(r.out, "0x8002  Log  qmp.test.kitchen.v1.Sink  QUEUE depth 4"));
    EXPECT_TRUE(contains(r.out, "message qmp.test.kitchen.v1.Sink {  // 256 bytes"));
    EXPECT_TRUE(contains(r.out, "repeated string tags = 13;"));
    EXPECT_TRUE(contains(r.out, "max_len 3, max_count 3"));
    EXPECT_TRUE(contains(r.out, "COLOR_BLUE=5"));
    EXPECT_TRUE(contains(r.out, "message qmp.test.kitchen.v1.Empty {}"));

    const std::string by_name = r.out;
    for (const char *spec : {"qmp.test.Kitchen", "qmp.test.kitchen.v1.Kitchen", "0777", "0x777",
                             "0777-0000", "0777-0000/v2", "qmp/0777-0000/v2"}) {
        r = ctl({"describe", spec});
        EXPECT_EQ(r.code, 0) << spec << ": " << r.err;
        EXPECT_EQ(r.out, by_name) << spec;
    }
}

TEST_F(Cli, DescribeJsonIsTheGeneratedReflectionDocument) {
    Result r = ctl({"describe", "Kitchen", "--json"});
    ASSERT_EQ(r.code, 0) << r.err;
    const std::string blob(reinterpret_cast<const char *>(qmp_test_kitchen_v1_Kitchen_reflect_json),
                           qmp_test_kitchen_v1_Kitchen_reflect_json_len);
    EXPECT_EQ(Json::parse(r.out), Json::parse(blob));
    // More than one REFLECT chunk travelled.
    EXPECT_GT(blob.size(), static_cast<size_t>(QMP_REFLECT_CHUNK_MAX));
}

TEST_F(Cli, DescribeOneMethodTopicOrMessage) {
    Result r = ctl({"describe", "Kitchen", "Fail"});
    ASSERT_EQ(r.code, 0) << r.err;
    EXPECT_TRUE(contains(r.out, "method Kitchen.Fail")) << r.out;
    EXPECT_TRUE(contains(r.out, "message qmp.test.kitchen.v1.Inner {  // 16 bytes"));
    EXPECT_FALSE(contains(r.out, "kitchen.v1.Sink"));
    EXPECT_TRUE(contains(r.out, "qmpctl call 0777-0000/v2 Fail '{\"a\":0,\"tag\":\"\"}'"));

    r = ctl({"describe", "Kitchen", "Echo", "--json"});
    ASSERT_EQ(r.code, 0) << r.err;
    Json j = Json::parse(r.out);
    EXPECT_EQ(j["method"]["id"], 1);
    EXPECT_TRUE(j["messages"].contains("qmp.test.kitchen.v1.Sink"));
    EXPECT_TRUE(j["messages"].contains("qmp.test.kitchen.v1.Inner")); // embedded by Sink
    EXPECT_EQ(j["example"]["inner"]["tag"], "");

    r = ctl({"describe", "Kitchen", "0x8002"});
    ASSERT_EQ(r.code, 0) << r.err;
    EXPECT_TRUE(contains(r.out, "topic Kitchen.Log")) << r.out;
    EXPECT_TRUE(contains(r.out, "qmpctl subscribe 0777-0000/v2 Log"));

    r = ctl({"describe", "Kitchen", "Inner"});
    ASSERT_EQ(r.code, 0) << r.err;
    EXPECT_EQ(r.out.rfind("message qmp.test.kitchen.v1.Inner {", 0), 0u) << r.out;
    EXPECT_EQ(ctl({"describe", "Kitchen", "qmp.test.kitchen.v1.Inner"}).out, r.out);

    r = ctl({"describe", "Kitchen", "Nope"});
    EXPECT_EQ(r.code, 1);
    EXPECT_TRUE(contains(r.err, "no method, topic or message 'Nope'")) << r.err;
}

TEST_F(Cli, UnknownAndAmbiguousServices) {
    Result r = ctl({"describe", "NoSuchService"});
    EXPECT_EQ(r.code, 1);
    EXPECT_TRUE(contains(r.err, "no service 'NoSuchService' is registered")) << r.err;
    r = ctl({"describe", "0666"});
    EXPECT_EQ(r.code, 1);
    r = ctl({"describe", "0777-0000/v9"});
    EXPECT_EQ(r.code, 1);
    EXPECT_TRUE(contains(r.err, "qmp/0777-0000/v9: not registered")) << r.err;

    Kitchen second;
    ASSERT_EQ(second.start(1), EOK);
    r = ctl({"describe", "Kitchen"});
    EXPECT_EQ(r.code, 1);
    EXPECT_TRUE(contains(r.err, "ambiguous")) << r.err;
    EXPECT_TRUE(contains(r.err, "qmp/0777-0000/v2 qmp/0777-0001/v2")) << r.err;
    EXPECT_EQ(ctl({"describe", "0777-0001"}).code, 0);
}

// ---- call ---------------------------------------------------------------------------------

TEST_F(Cli, CallRoundTripsEveryFieldShape) {
    const char *args = R"({"flag":true,"i32":-5,"u32":4000000000,"i64":-9000000000,)"
                       R"("u64":18446744073709551615,"f":0.1,"d":2.5,"color":"COLOR_BLUE",)"
                       R"("name":"hello","blob":"AQID","inner":{"a":7,"tag":"in"},)"
                       R"("nums":[1,2,3],"tags":["a","bcd"],)"
                       R"("inners":[{"a":1,"tag":"x"},{"a":2,"tag":"yyyyy"}],"bits":[true],)"
                       R"("colors":["COLOR_RED",7],"chunks":["/w==","AAE="],"reals":[1.5,-0.25]})";
    Result r = ctl({"call", "Kitchen", "Echo", args});
    ASSERT_EQ(r.code, 0) << r.err;
    EXPECT_EQ(r.out, std::string(args) + "\n");
    EXPECT_EQ(r.err, "");
    EXPECT_EQ(kitchen_.echoes.load(), 1);

    // The same arguments from stdin and from a file; --pretty only changes whitespace.
    Result piped = ctl({"call", "Kitchen", "Echo", "-"}, args);
    EXPECT_EQ(piped.out, r.out) << piped.err;
    const std::string file = g_dir + "/args.json";
    std::ofstream(file) << args;
    Result from_file = ctl({"call", "0777", "1", "@" + file, "--pretty"});
    EXPECT_EQ(from_file.code, 0) << from_file.err;
    EXPECT_GT(lines(from_file.out).size(), 20u);
    EXPECT_EQ(Json::parse(from_file.out), Json::parse(args));
    (void)unlink(file.c_str());
}

TEST_F(Cli, CallWithKeyValueArgumentsAndDefaults) {
    Result r = ctl({"call", "Kitchen", "Echo", "i32=-3", "name=bob", "inner.a=4", "inner.tag=t",
                    "nums=[9,8]", "flag=true", "color=COLOR_RED", "f=2.5"});
    ASSERT_EQ(r.code, 0) << r.err;
    const Json j = Json::parse(r.out);
    EXPECT_EQ(j["i32"], -3);
    EXPECT_EQ(j["name"], "bob");
    EXPECT_EQ(j["inner"], Json::parse(R"({"a":4,"tag":"t"})"));
    EXPECT_EQ(j["nums"], Json::parse("[9,8]"));
    EXPECT_EQ(j["flag"], true);
    EXPECT_EQ(j["color"], "COLOR_RED");
    EXPECT_EQ(j["f"], 2.5);
    EXPECT_EQ(j["u64"], 0);

    // No arguments at all: every field at its default.
    r = ctl({"call", "Kitchen", "Echo"});
    ASSERT_EQ(r.code, 0) << r.err;
    EXPECT_EQ(Json::parse(r.out)["name"], "");
    // A numeric-looking string for a string field must be quoted as JSON.
    r = ctl({"call", "Kitchen", "Echo", "name=\"42\""});
    ASSERT_EQ(r.code, 0) << r.err;
    EXPECT_EQ(Json::parse(r.out)["name"], "42");
}

TEST_F(Cli, CallEmptyRequestAndReply) {
    Result r = ctl({"call", "Kitchen", "Ping"});
    ASSERT_EQ(r.code, 0) << r.err;
    EXPECT_EQ(r.out, "{}\n");
    EXPECT_EQ(ctl({"call", "Kitchen", "Ping", "{}"}).code, 0);
    EXPECT_EQ(kitchen_.pings.load(), 2);
    r = ctl({"call", "Kitchen", "Ping", "x=1"});
    EXPECT_EQ(r.code, 2);
    EXPECT_TRUE(contains(r.err, "x: no such field")) << r.err;
    EXPECT_EQ(kitchen_.pings.load(), 2);
}

TEST_F(Cli, CallOneWayPrintsNothing) {
    Result r = ctl({"call", "Kitchen", "Fire", "a=5"});
    ASSERT_EQ(r.code, 0) << r.err;
    EXPECT_EQ(r.out, "");
    // The handler runs after the receipt reply: wait for it.
    for (int i = 0; i < 500; i++) {
        {
            std::lock_guard<std::mutex> lk(kitchen_.mu);
            if (!kitchen_.fired.empty()) {
                break;
            }
        }
        std::this_thread::sleep_for(10ms);
    }
    std::lock_guard<std::mutex> lk(kitchen_.mu);
    EXPECT_EQ(kitchen_.fired, std::vector<int32_t>{5});
}

TEST_F(Cli, CallReportsApplicationStatusAndTransportErrors) {
    Result r = ctl({"call", "Kitchen", "Fail", "a=3", "tag=x"});
    EXPECT_EQ(r.code, 1);
    EXPECT_EQ(r.out, "");
    EXPECT_TRUE(contains(r.err, "Kitchen.Fail returned status 3 (INVALID_ARGUMENT)")) << r.err;

    r = ctl({"call", "Kitchen", "Fail", "a=0", "tag=ok"});
    EXPECT_EQ(r.code, 0) << r.err;
    EXPECT_EQ(r.out, "{\"a\":0,\"tag\":\"ok\"}\n");

    r = ctl({"call", "Kitchen", "Fail", "a=-1"});
    EXPECT_EQ(r.code, 1);
    EXPECT_TRUE(contains(r.err, "Kitchen.Fail failed: ")) << r.err;
    EXPECT_TRUE(contains(r.err, "(errno " + std::to_string(EXDEV) + ")")) << r.err;
}

TEST_F(Cli, CallRejectsBadInputBeforeSendingAnything) {
    Result r = ctl({"call", "Kitchen", "Echo", R"({"name":"much too long for it"})"});
    EXPECT_EQ(r.code, 2);
    EXPECT_TRUE(contains(r.err, "name: string is 20 bytes, max_len is 12")) << r.err;
    r = ctl({"call", "Kitchen", "Echo", "{not json"});
    EXPECT_EQ(r.code, 2);
    r = ctl({"call", "Kitchen", "Echo", "[1]"});
    EXPECT_EQ(r.code, 2);
    r = ctl({"call", "Kitchen", "Echo", "@" + g_dir + "/missing.json"});
    EXPECT_EQ(r.code, 2);
    r = ctl({"call", "Kitchen", "Echo", "inner=1", "inner.a=2"});
    EXPECT_EQ(r.code, 2);
    EXPECT_EQ(kitchen_.echoes.load(), 0);

    r = ctl({"call", "Kitchen", "Bake"});
    EXPECT_EQ(r.code, 1);
    EXPECT_TRUE(contains(r.err, "no method 'Bake' (methods: Echo Fail Ping Fire Publish)"))
        << r.err;
    EXPECT_EQ(ctl({"call", "Kitchen"}).code, 2);
}

// ---- subscribe ----------------------------------------------------------------------------

// Run `subscribe` on a thread and keep publishing through `publish` until it returns.
template <typename Fn> Result subscribe_while(std::vector<std::string> args, Fn publish) {
    Result r;
    std::atomic<bool> done{false};
    std::thread sub([&] {
        r = ctl(std::move(args));
        done = true;
    });
    for (int i = 1; !done && i < 2000; i++) {
        publish(i);
        std::this_thread::sleep_for(5ms);
    }
    sub.join();
    return r;
}

TEST_F(Cli, SubscribePrintsOneJsonLinePerUpdate) {
    Result r = subscribe_while({"subscribe", "Kitchen", "Tick", "--count", "3"}, [&](int i) {
        qmp_test_kitchen_v1_Inner in;
        std::memset(&in, 0, sizeof(in));
        in.a = i;
        in.tag.len = 2;
        std::memcpy(in.tag.data, "tk", 2);
        EXPECT_EQ(qmp_test_kitchen_v1_Kitchen_publish_Tick(&kitchen_.server, &in), EOK);
    });
    ASSERT_EQ(r.code, 0) << r.err;
    const std::vector<std::string> ls = lines(r.out);
    ASSERT_EQ(ls.size(), 3u) << r.out;
    uint64_t prev_seq = 0;
    uint64_t prev_rx = 0;
    for (const std::string &l : ls) {
        const Json j = Json::parse(l);
        EXPECT_EQ(j["topic"], "Tick");
        EXPECT_GT(j["seq"].get<uint64_t>(), prev_seq);
        EXPECT_GE(j["rx_ns"].get<uint64_t>(), prev_rx);
        // The sample's own counter equals the publish sequence number: LATEST delivers
        // the newest sample and accounts for the ones it skipped.
        EXPECT_EQ(j["sample"]["a"], j["seq"]);
        EXPECT_EQ(j["sample"]["tag"], "tk");
        if (prev_seq != 0) { // the first line follows whatever was published before
            EXPECT_EQ(j["dropped"].get<uint64_t>(), j["seq"].get<uint64_t>() - prev_seq - 1);
        }
        prev_seq = j["seq"].get<uint64_t>();
        prev_rx = j["rx_ns"].get<uint64_t>();
    }
    // Wall-clock receive time, in nanoseconds.
    EXPECT_GT(prev_rx, 1600000000ULL * 1000000000ULL);
    EXPECT_TRUE(contains(r.err, "3 updates received")) << r.err;
}

TEST_F(Cli, SubscribeToAQueueTopicByIdSeesEveryUpdateInOrder) {
    // Publishes arrive through qmpctl itself: Publish(a=2) puts two samples on Log.
    Result r = subscribe_while({"subscribe", "0777-0000/v2", "0x8002", "--count", "4"}, [&](int i) {
        if (i % 20 == 1) {
            EXPECT_EQ(ctl({"call", "Kitchen", "Publish", "a=2"}).code, 0);
        }
    });
    ASSERT_EQ(r.code, 0) << r.err;
    const std::vector<std::string> ls = lines(r.out);
    ASSERT_EQ(ls.size(), 4u) << r.out;
    uint64_t first = Json::parse(ls[0])["seq"].get<uint64_t>();
    for (size_t i = 0; i < ls.size(); i++) {
        const Json j = Json::parse(ls[i]);
        EXPECT_EQ(j["seq"].get<uint64_t>(), first + i);
        EXPECT_EQ(j["dropped"], 0);
        EXPECT_EQ(j["sample"]["i32"].get<uint64_t>(), (first + i - 1) % 2);
    }
}

TEST_F(Cli, SubscribeStopsOnDurationAndOnRequest) {
    const auto t0 = std::chrono::steady_clock::now();
    Result r = ctl({"subscribe", "Kitchen", "Tick", "--duration", "150"});
    const auto dt = std::chrono::steady_clock::now() - t0;
    EXPECT_EQ(r.code, 0) << r.err;
    EXPECT_EQ(r.out, "");
    EXPECT_GE(dt, 150ms);
    EXPECT_LT(dt, 5s);
    EXPECT_TRUE(contains(r.err, "0 updates received, 0 dropped")) << r.err;

    // SIGINT/SIGTERM in the real binary call request_stop().
    std::atomic<bool> done{false};
    std::thread sub([&] {
        r = ctl({"subscribe", "Kitchen", "Tick"});
        done = true;
    });
    for (int i = 0; i < 1000 && !done; i++) {
        std::this_thread::sleep_for(20ms);
        qmpctl::request_stop();
    }
    sub.join();
    EXPECT_EQ(r.code, 0) << r.err;

    r = ctl({"subscribe", "Kitchen", "Nope"});
    EXPECT_EQ(r.code, 1);
    EXPECT_TRUE(contains(r.err, "no topic 'Nope' (topics: Tick Log)")) << r.err;
    EXPECT_EQ(ctl({"subscribe", "Kitchen"}).code, 2);
}

// Counts complete lines as they are written, so a test can react to qmpctl's output
// while the command is still running.
class LineCountingBuf : public std::stringbuf {
  public:
    std::atomic<int> lines{0};

  protected:
    std::streamsize xsputn(const char *s, std::streamsize n) override {
        lines += static_cast<int>(std::count(s, s + n, '\n'));
        return std::stringbuf::xsputn(s, n);
    }
    int_type overflow(int_type ch) override {
        if (ch == '\n') {
            lines++;
        }
        return std::stringbuf::overflow(ch);
    }
};

TEST(CliRestart, SubscribeSurvivesAServerRestart) {
    auto server = std::make_unique<Kitchen>();
    ASSERT_EQ(server->start(), EOK);
    LineCountingBuf buf;
    std::ostream out(&buf);
    std::ostringstream err;
    std::istringstream in;
    int code = -1;
    std::atomic<bool> done{false};
    std::thread sub([&] {
        code = qmpctl::run(
            {"--transport", transport_name(), "subscribe", "Kitchen", "Tick", "--count", "12"}, in,
            out, err);
        done = true;
    });
    int restarted_at = 0;
    for (int i = 1; !done && i < 4000; i++) {
        if (restarted_at == 0 && buf.lines >= 4) {
            // Mid-stream: stop the server and bring a new one up under the same name.
            server.reset();
            server = std::make_unique<Kitchen>();
            ASSERT_EQ(server->start(), EOK);
            restarted_at = i;
        }
        qmp_test_kitchen_v1_Inner sample;
        std::memset(&sample, 0, sizeof(sample));
        sample.a = i;
        (void)qmp_test_kitchen_v1_Kitchen_publish_Tick(&server->server, &sample);
        std::this_thread::sleep_for(5ms);
    }
    sub.join();
    ASSERT_EQ(code, 0) << err.str();
    ASSERT_GT(restarted_at, 0);
    const std::vector<std::string> ls = lines(buf.str());
    ASSERT_EQ(ls.size(), 12u) << buf.str();
    EXPECT_TRUE(contains(err.str(), "went away, reconnecting")) << err.str();
    EXPECT_TRUE(contains(err.str(), "reconnected")) << err.str();
    // Samples from before and after the restart both arrived.
    EXPECT_LT(Json::parse(ls.front())["sample"]["a"].get<int>(), restarted_at);
    EXPECT_GE(Json::parse(ls.back())["sample"]["a"].get<int>(), restarted_at);
}

// ---- reflection from a file -----------------------------------------------------------------

TEST(CliReflectFile, ServiceWithoutReflectionNeedsAndUsesTheFile) {
    Kitchen mute;
    ASSERT_EQ(mute.start(0, false), EOK);
    Result r = ctl({"call", "0777", "Ping"});
    EXPECT_EQ(r.code, 1);
    EXPECT_TRUE(contains(r.err, "no reflection data (pass --reflect")) << r.err;
    // Its name is unknown without the file, too.
    EXPECT_EQ(ctl({"call", "Kitchen", "Ping"}).code, 1);

    const std::string file = g_dir + "/kitchen.reflect.json";
    std::ofstream(file, std::ios::binary)
        .write(reinterpret_cast<const char *>(qmp_test_kitchen_v1_Kitchen_reflect_json),
               static_cast<std::streamsize>(qmp_test_kitchen_v1_Kitchen_reflect_json_len));
    for (const char *spec : {"0777", "Kitchen"}) {
        r = ctl({"--reflect", file, "call", spec, "Fail", "tag=file"});
        EXPECT_EQ(r.code, 0) << spec << ": " << r.err;
        EXPECT_EQ(r.out, "{\"a\":0,\"tag\":\"file\"}\n");
    }
    r = ctl({"describe", "0777", "--reflect", file});
    EXPECT_EQ(r.code, 0) << r.err;
    EXPECT_TRUE(contains(r.out, "described from a --reflect file")) << r.out;
    r = ctl({"list", "--reflect", file});
    EXPECT_TRUE(contains(r.out, "qmp.test.Kitchen  2.3")) << r.out;

    std::ofstream(file) << "{\"qmp_reflect_version\": 1}";
    r = ctl({"--reflect", file, "call", "0777", "Ping"});
    EXPECT_EQ(r.code, 1);
    EXPECT_TRUE(contains(r.err, "kitchen.reflect.json: reflection data:")) << r.err;
    EXPECT_EQ(ctl({"--reflect", g_dir + "/missing.json", "list"}).code, 1);
    (void)unlink(file.c_str());
}

// ---- transports and the command line --------------------------------------------------------

TEST(CliFake, InProcessTransportNeedsAFullAddress) {
    (void)transport_name();
    Kitchen k;
    ASSERT_EQ(k.start(0, true, "fake"), EOK);
    std::istringstream in;
    std::ostringstream out;
    std::ostringstream err;
    // The fake transport has no namespace to walk: list is empty, names cannot resolve.
    EXPECT_EQ(qmpctl::run({"--transport", "fake", "list"}, in, out, err), 0);
    EXPECT_EQ(out.str(), "");
    EXPECT_EQ(qmpctl::run({"--transport", "fake", "call", "Kitchen", "Ping"}, in, out, err), 1);
    EXPECT_EQ(
        qmpctl::run({"--transport", "fake", "call", "0777-0000/v2", "Fail", "tag=f"}, in, out, err),
        0)
        << err.str();
    EXPECT_EQ(out.str(), "{\"a\":0,\"tag\":\"f\"}\n");
}

TEST(CliUsage, HelpVersionAndErrors) {
    std::istringstream in;
    auto run = [&](std::vector<std::string> args) {
        std::ostringstream out;
        std::ostringstream err;
        Result r;
        r.code = qmpctl::run(args, in, out, err);
        r.out = out.str();
        r.err = err.str();
        return r;
    };
    Result r = run({"--help"});
    EXPECT_EQ(r.code, 0);
    EXPECT_TRUE(contains(r.out, "usage: qmpctl"));
    for (const char *cmd : {"list", "describe", "call", "subscribe"}) {
        EXPECT_TRUE(contains(r.out, std::string("  ") + cmd)) << cmd;
    }
    r = run({"--version"});
    EXPECT_EQ(r.code, 0);
    EXPECT_EQ(r.out.rfind(std::string("qmpctl ") + QMP_VERSION_STRING, 0), 0u) << r.out;
    EXPECT_TRUE(contains(r.out, std::string("qmpctl ") + qmp_build_version() + " (wire " +
                                    std::to_string(QMP_WIRE_VERSION) +
                                    ", reflect 1) build=" + qmp_build_id() +
                                    " transport=" + qmp_transport_default()->name + "\n"))
        << r.out;
    r = run({"--version", "--json"});
    EXPECT_EQ(r.code, 0);
    {
        Json j = Json::parse(r.out);
        EXPECT_EQ(j["version"], QMP_VERSION_STRING);
        EXPECT_EQ(j["build_version"], qmp_build_version());
        EXPECT_EQ(j["wire"], QMP_WIRE_VERSION);
        EXPECT_EQ(j["reflect"], 1);
        EXPECT_EQ(j["build"], qmp_build_id());
        EXPECT_EQ(j["transport"], qmp_transport_default()->name);
    }

    r = run({});
    EXPECT_EQ(r.code, 2);
    EXPECT_TRUE(contains(r.err, "usage: qmpctl"));
    r = run({"frobnicate"});
    EXPECT_EQ(r.code, 2);
    EXPECT_TRUE(contains(r.err, "unknown command 'frobnicate'"));
    EXPECT_EQ(run({"list", "--bogus"}).code, 2);
    EXPECT_EQ(run({"list", "--timeout"}).code, 2);
    EXPECT_EQ(run({"list", "--timeout", "soon"}).code, 2);
    r = run({"--transport", "carrier-pigeon", "list"});
    EXPECT_EQ(r.code, 2);
    EXPECT_TRUE(contains(r.err, "transport 'carrier-pigeon' is not available")) << r.err;
    EXPECT_EQ(run({"describe"}).code, 2);
}

} // namespace
