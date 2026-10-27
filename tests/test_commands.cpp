#include <gtest/gtest.h>

#include <limits>
#include <sstream>
#include <string>

#include "lob/commands.hpp"
#include "lob/generator.hpp"

using namespace lob;

namespace {
Command new_cmd(OrderId id, Side side, Price price, Quantity qty) {
    return NewCommand{Order{id, side, price, qty}};
}
}  // namespace

TEST(Parse, AllCommandForms) {
    const auto r = parse_commands("NEW 1 BUY 100 10\nNEW 2 sell 101 5\nNEW 3 B 99 1\nNEW 4 S 98 2\n"
                                  "CANCEL 1\nSNAPSHOT\nSNAPSHOT 3\n");
    ASSERT_TRUE(r.ok()) << r.error->message;
    ASSERT_EQ(r.commands.size(), 7u);
    EXPECT_EQ(r.commands[0], new_cmd(1, Side::Buy, 100, 10));
    EXPECT_EQ(r.commands[1], new_cmd(2, Side::Sell, 101, 5));
    EXPECT_EQ(r.commands[2], new_cmd(3, Side::Buy, 99, 1));
    EXPECT_EQ(r.commands[3], new_cmd(4, Side::Sell, 98, 2));
    EXPECT_EQ(r.commands[4], Command{CancelCommand{1}});
    EXPECT_EQ(r.commands[5], Command{SnapshotCommand{}});
    EXPECT_EQ(r.commands[6], Command{SnapshotCommand{3}});
}

TEST(Parse, CommentsBlankLinesWhitespaceAndCrlf) {
    const auto r = parse_commands("# header\n\n   \t\nNEW 1 BUY 100 10 # trailing\r\n  cancel   1  \r\n");
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.commands.size(), 2u);
}

TEST(Parse, NegativeAndExtremePricesParseSoTheEngineCanRejectThem) {
    const auto r = parse_commands("NEW 1 BUY -5 10\nNEW 2 BUY 0 10\nNEW 3 SELL 9223372036854775807 1\n");
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(std::get<NewCommand>(r.commands[0]).order.price, -5);
}

TEST(Parse, MaxValuesRoundTrip) {
    const Order o{std::numeric_limits<OrderId>::max(), Side::Sell, std::numeric_limits<Price>::max(),
                  std::numeric_limits<Quantity>::max()};
    const auto r = parse_commands(format_command(NewCommand{o}));
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.commands.at(0), Command{NewCommand{o}});
}

struct BadCase {
    const char* text;
    std::size_t line;
};

TEST(Parse, MalformedInputIsAnErrorWithLineNumber) {
    const BadCase cases[] = {
        {"HELLO 1", 1},
        {"NEW 1 BUY 100", 1},                          // too few args
        {"NEW 1 BUY 100 10 extra", 1},                 // too many args
        {"NEW x BUY 100 10", 1},                       // non-numeric id
        {"NEW -1 BUY 100 10", 1},                      // negative id
        {"NEW 1 SIDEWAYS 100 10", 1},                  // bad side
        {"NEW 1 BUY 1.5 10", 1},                       // float price
        {"NEW 1 BUY 100 -3", 1},                       // negative quantity
        {"NEW 1 BUY 100 +3", 1},                       // explicit plus
        {"NEW 1 BUY 100 18446744073709551616", 1},     // quantity overflow
        {"NEW 1 BUY 9223372036854775808 1", 1},        // price overflow
        {"CANCEL", 1},
        {"CANCEL 1 2", 1},
        {"CANCEL abc", 1},
        {"SNAPSHOT 1 2", 1},
        {"SNAPSHOT -1", 1},
        {"NEW 1 BUY 100 10\n\nbogus", 3},              // line number counts blanks
        {"NEW 1 BUY 100 10x", 1},                      // trailing junk in a number
    };
    for (const BadCase& c : cases) {
        const auto r = parse_commands(c.text);
        ASSERT_FALSE(r.ok()) << "accepted: " << c.text;
        EXPECT_EQ(r.error->line, c.line) << c.text;
        EXPECT_FALSE(r.error->message.empty());
    }
}

TEST(Parse, ErrorKeepsCommandsParsedBeforeIt) {
    const auto r = parse_commands("NEW 1 BUY 100 10\nCANCEL 1\nBOGUS\nCANCEL 2\n");
    ASSERT_FALSE(r.ok());
    EXPECT_EQ(r.commands.size(), 2u);
    EXPECT_EQ(r.error->line, 3u);
}

TEST(Parse, EmptyInputIsValid) {
    const auto r = parse_commands("");
    EXPECT_TRUE(r.ok());
    EXPECT_TRUE(r.commands.empty());
}

TEST(Format, GeneratedCommandsRoundTripExactly) {
    GeneratorConfig cfg;
    cfg.seed = 5;
    cfg.count = 2000;
    cfg.snapshot_every = 100;
    const auto cmds = generate(cfg);
    std::ostringstream os;
    write_commands(os, cmds);
    const auto back = parse_commands(os.str());
    ASSERT_TRUE(back.ok());
    EXPECT_EQ(back.commands, cmds);
}

// ---------------------------------------------------------- Phase 6 syntax ---------

TEST(Parse, MarketOrdersAndTimeInForce) {
    const auto r = parse_commands("NEW 1 BUY MKT 5\nNEW 2 sell mkt 3 FOK\nNEW 3 BUY 100 5 IOC\nNEW 4 BUY 100 5 gtc\nNEW 5 BUY MKT 5 GTC\n");
    ASSERT_TRUE(r.ok()) << r.error->message;
    ASSERT_EQ(r.commands.size(), 5u);
    EXPECT_EQ(r.commands[0], Command{NewCommand{Order::market(1, Side::Buy, 5)}});  // MKT defaults to IOC
    EXPECT_EQ(r.commands[1], Command{NewCommand{Order::market(2, Side::Sell, 3, TimeInForce::FOK)}});
    EXPECT_EQ(r.commands[2], Command{NewCommand{Order(3, Side::Buy, 100, 5, TimeInForce::IOC)}});
    EXPECT_EQ(r.commands[3], Command{NewCommand{Order(4, Side::Buy, 100, 5)}});
    // A GTC market order parses (so the engine's rejection is replayable) but is never valid.
    EXPECT_EQ(r.commands[4], Command{NewCommand{Order::market(5, Side::Buy, 5, TimeInForce::GTC)}});
}

TEST(Parse, Amend) {
    const auto r = parse_commands("AMEND 7 101 3\namend 8 -5 1\n");
    ASSERT_TRUE(r.ok());
    const Command first = AmendCommand{7, 101, 3};
    const Command second = AmendCommand{8, -5, 1};  // bad price parses; the engine rejects it
    EXPECT_EQ(r.commands[0], first);
    EXPECT_EQ(r.commands[1], second);
}

TEST(Parse, Phase6ErrorsHaveLineNumbers) {
    const BadCase cases[] = {
        {"NEW 1 BUY 100 5 DAY", 1},      // unknown time in force
        {"NEW 1 BUY 100 5 IOC extra", 1},
        {"NEW 1 BUY MARKET 5", 1},       // only MKT is accepted
        {"NEW 1 BUY MKT", 1},
        {"AMEND 1 100", 1},
        {"AMEND 1 100 5 6", 1},
        {"AMEND x 100 5", 1},
        {"AMEND 1 1.5 5", 1},
        {"AMEND 1 100 -5", 1},
    };
    for (const BadCase& c : cases) {
        const auto r = parse_commands(c.text);
        ASSERT_FALSE(r.ok()) << "accepted: " << c.text;
        EXPECT_EQ(r.error->line, c.line) << c.text;
    }
}

TEST(Format, Phase6CommandsRoundTrip) {
    const std::vector<Command> cmds{
        NewCommand{Order::market(1, Side::Buy, 5)},
        NewCommand{Order::market(2, Side::Sell, 3, TimeInForce::FOK)},
        NewCommand{Order::market(3, Side::Buy, 3, TimeInForce::GTC)},
        NewCommand{Order(4, Side::Buy, 100, 5, TimeInForce::IOC)},
        NewCommand{Order(5, Side::Sell, 100, 5, TimeInForce::FOK)},
        NewCommand{Order(6, Side::Sell, 100, 5)},
        AmendCommand{7, 101, 3},
    };
    std::ostringstream os;
    write_commands(os, cmds);
    const auto back = parse_commands(os.str());
    ASSERT_TRUE(back.ok()) << back.error->message;
    EXPECT_EQ(back.commands, cmds);
    EXPECT_EQ(format_command(cmds[5]), "NEW 6 SELL 100 5");  // plain limit GTC keeps the short form
    EXPECT_EQ(format_command(cmds[0]), "NEW 1 BUY MKT 5 IOC");
}

TEST(Format, GeneratedFilesWithExtensionsRoundTrip) {
    GeneratorConfig cfg;
    cfg.seed = 9;
    cfg.count = 3000;
    cfg.ioc_percent = 15;
    cfg.fok_percent = 10;
    cfg.market_percent = 10;
    cfg.amend_percent = 15;
    cfg.snapshot_every = 200;
    const auto cmds = generate(cfg);
    std::size_t amends = 0, markets = 0;
    for (const Command& c : cmds) {
        amends += std::holds_alternative<AmendCommand>(c);
        if (const auto* n = std::get_if<NewCommand>(&c)) markets += n->order.type == OrderType::Market;
    }
    EXPECT_GT(amends, 100u);
    EXPECT_GT(markets, 100u);
    std::ostringstream os;
    write_commands(os, cmds);
    const auto back = parse_commands(os.str());
    ASSERT_TRUE(back.ok()) << back.error->message;
    EXPECT_EQ(back.commands, cmds);
}
