#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "descriptors.hpp"
#include "keyboard_scancode.hpp"
#include "neutrino.h"
#include "syscall.hpp"

namespace {

constexpr uint32_t kDescConsole =
    static_cast<uint32_t>(descriptor_defs::Type::Console);
constexpr uint32_t kDescKeyboard =
    static_cast<uint32_t>(descriptor_defs::Type::Keyboard);

constexpr uint32_t kDefaultFg = 0xFFF2F4F8u;
constexpr uint32_t kDefaultBg = 0xFF101418u;
constexpr uint32_t kHeaderFg = 0xFF101418u;
constexpr uint32_t kHeaderBg = 0xFF9FE7D7u;
constexpr uint32_t kRedFg = 0xFFFF7A7Au;
constexpr uint32_t kBlackFg = 0xFFE7EDF3u;
constexpr uint32_t kDimFg = 0xFF8B99A8u;
constexpr uint32_t kSelectFg = 0xFF101418u;
constexpr uint32_t kSelectBg = 0xFFFFD36Eu;

constexpr size_t kTableauCount = 7;
constexpr size_t kFoundationCount = 4;
constexpr size_t kMaxPile = 52;
constexpr size_t kMaxUndo = 32;
constexpr size_t kCommandSize = 80;

enum class MovePlace {
    Unknown,
    Waste,
    Foundation,
    Tableau,
};

struct Pile {
    uint8_t cards[kMaxPile];
    bool face_up[kMaxPile];
    size_t count;
};

struct Game {
    Pile stock;
    Pile waste;
    Pile foundations[kFoundationCount];
    Pile tableau[kTableauCount];
    uint32_t moves;
    uint32_t seed;
};

struct App {
    Game game;
    Game undo[kMaxUndo];
    size_t undo_count;
    char status[96];
    MovePlace selected_place;
    size_t selected_pile;
    bool show_help;
    bool quit;
};

App g_app;

void write_text(long console, const char* text) {
    if (console >= 0 && text != nullptr) {
        descriptor_write(static_cast<uint32_t>(console), text, strlen(text));
    }
}

void write_char(long console, char ch) {
    descriptor_write(static_cast<uint32_t>(console), &ch, 1);
}

void set_color(long console, uint32_t fg, uint32_t bg) {
    descriptor_defs::ColorPair colors{fg, bg};
    descriptor_set_property(
        static_cast<uint32_t>(console),
        static_cast<uint32_t>(descriptor_defs::Property::ConsoleColor),
        &colors,
        sizeof(colors));
}

void clear_console(long console) {
    descriptor_set_property(
        static_cast<uint32_t>(console),
        static_cast<uint32_t>(descriptor_defs::Property::ConsoleClear),
        nullptr,
        0);
}

void defer_updates(long console, bool deferred) {
    uint8_t value = deferred ? 1 : 0;
    descriptor_set_property(
        static_cast<uint32_t>(console),
        static_cast<uint32_t>(descriptor_defs::Property::ConsoleUpdate),
        &value,
        sizeof(value));
}

void append_text(char* out, size_t out_size, const char* text) {
    if (out == nullptr || out_size == 0 || text == nullptr) return;
    size_t len = strlen(out);
    while (*text != '\0' && len + 1 < out_size) {
        out[len++] = *text++;
    }
    out[len] = '\0';
}

void append_uint(char* out, size_t out_size, uint32_t value) {
    char tmp[12];
    size_t pos = 0;
    if (value == 0) {
        tmp[pos++] = '0';
    } else {
        while (value != 0 && pos < sizeof(tmp)) {
            tmp[pos++] = static_cast<char>('0' + (value % 10u));
            value /= 10u;
        }
    }
    size_t len = strlen(out);
    while (pos != 0 && len + 1 < out_size) {
        out[len++] = tmp[--pos];
    }
    out[len] = '\0';
}

void set_status(App& app, const char* text) {
    strlcpy(app.status, text, sizeof(app.status));
}

uint8_t rank(uint8_t card) {
    return static_cast<uint8_t>((card % 13u) + 1u);
}

uint8_t suit(uint8_t card) {
    return static_cast<uint8_t>(card / 13u);
}

bool is_red(uint8_t card) {
    uint8_t s = suit(card);
    return s == 0 || s == 1;
}

const char* rank_text(uint8_t r) {
    static const char* names[] = {
        "?", "A", "2", "3", "4", "5", "6",
        "7", "8", "9", "10", "J", "Q", "K",
    };
    return names[r <= 13 ? r : 0];
}

char suit_text(uint8_t s) {
    static const char names[] = {'H', 'D', 'C', 'S'};
    return names[s < 4 ? s : 0];
}

void pile_push(Pile& pile, uint8_t card, bool up) {
    if (pile.count >= kMaxPile) return;
    pile.cards[pile.count] = card;
    pile.face_up[pile.count] = up;
    ++pile.count;
}

uint8_t pile_pop(Pile& pile) {
    if (pile.count == 0) return 0;
    --pile.count;
    return pile.cards[pile.count];
}

uint32_t next_random(uint32_t& state) {
    state = state * 1664525u + 1013904223u;
    return state;
}

uint32_t initial_seed() {
    NeutrinoWallTime now{};
    if (neutrino_get_time(&now)) {
        uint64_t mixed = now.unix_seconds ^ now.nanoseconds;
        return static_cast<uint32_t>(mixed ^ (mixed >> 32));
    }
    return 0x51A7EA5Eu;
}

void save_undo(App& app) {
    if (app.undo_count == kMaxUndo) {
        for (size_t i = 1; i < kMaxUndo; ++i) {
            app.undo[i - 1] = app.undo[i];
        }
        app.undo_count = kMaxUndo - 1;
    }
    app.undo[app.undo_count++] = app.game;
}

void reveal_tail(Pile& pile) {
    if (pile.count > 0) {
        pile.face_up[pile.count - 1] = true;
    }
}

void deal(Game& game, uint32_t seed) {
    game = {};
    game.seed = seed == 0 ? 0x51A7EA5Eu : seed;

    uint8_t deck[52];
    for (uint8_t i = 0; i < 52; ++i) deck[i] = i;
    for (size_t i = 51; i > 0; --i) {
        size_t j = static_cast<size_t>(next_random(game.seed) % (i + 1));
        uint8_t tmp = deck[i];
        deck[i] = deck[j];
        deck[j] = tmp;
    }

    size_t cursor = 0;
    for (size_t col = 0; col < kTableauCount; ++col) {
        for (size_t row = 0; row <= col; ++row) {
            pile_push(game.tableau[col], deck[cursor++], row == col);
        }
    }
    while (cursor < 52) {
        pile_push(game.stock, deck[cursor++], false);
    }
}

bool can_place_on_tableau(uint8_t moving, const Pile& dest) {
    if (dest.count == 0) {
        return rank(moving) == 13;
    }
    uint8_t top = dest.cards[dest.count - 1];
    return dest.face_up[dest.count - 1] && is_red(moving) != is_red(top) &&
           rank(moving) + 1 == rank(top);
}

bool can_place_on_foundation(uint8_t moving, const Pile& dest) {
    if (dest.count == 0) {
        return rank(moving) == 1;
    }
    uint8_t top = dest.cards[dest.count - 1];
    return suit(moving) == suit(top) && rank(moving) == rank(top) + 1;
}

bool draw_stock(App& app) {
    Game& g = app.game;
    if (g.stock.count > 0) {
        save_undo(app);
        uint8_t card = pile_pop(g.stock);
        pile_push(g.waste, card, true);
        ++g.moves;
        set_status(app, "Drew from stock.");
        return true;
    }
    if (g.waste.count == 0) {
        set_status(app, "Stock and waste are both empty.");
        return false;
    }
    save_undo(app);
    while (g.waste.count > 0) {
        uint8_t card = pile_pop(g.waste);
        pile_push(g.stock, card, false);
    }
    ++g.moves;
    set_status(app, "Recycled waste into stock.");
    return true;
}

bool waste_to_tableau(App& app, size_t dest) {
    Game& g = app.game;
    if (g.waste.count == 0 || dest >= kTableauCount) {
        set_status(app, "No waste card or bad destination.");
        return false;
    }
    uint8_t card = g.waste.cards[g.waste.count - 1];
    if (!can_place_on_tableau(card, g.tableau[dest])) {
        set_status(app, "That waste card cannot go there.");
        return false;
    }
    save_undo(app);
    pile_pop(g.waste);
    pile_push(g.tableau[dest], card, true);
    ++g.moves;
    set_status(app, "Moved waste to tableau.");
    return true;
}

bool waste_to_foundation(App& app) {
    Game& g = app.game;
    if (g.waste.count == 0) {
        set_status(app, "Waste is empty.");
        return false;
    }
    uint8_t card = g.waste.cards[g.waste.count - 1];
    size_t dest = suit(card);
    if (!can_place_on_foundation(card, g.foundations[dest])) {
        set_status(app, "Waste card cannot go to foundation.");
        return false;
    }
    save_undo(app);
    pile_pop(g.waste);
    pile_push(g.foundations[dest], card, true);
    ++g.moves;
    set_status(app, "Moved waste to foundation.");
    return true;
}

bool tableau_to_foundation(App& app, size_t src) {
    Game& g = app.game;
    if (src >= kTableauCount || g.tableau[src].count == 0) {
        set_status(app, "That tableau pile is empty.");
        return false;
    }
    Pile& pile = g.tableau[src];
    if (!pile.face_up[pile.count - 1]) {
        set_status(app, "Top tableau card is face down.");
        return false;
    }
    uint8_t card = pile.cards[pile.count - 1];
    size_t dest = suit(card);
    if (!can_place_on_foundation(card, g.foundations[dest])) {
        set_status(app, "That card cannot go to foundation.");
        return false;
    }
    save_undo(app);
    pile_pop(pile);
    reveal_tail(pile);
    pile_push(g.foundations[dest], card, true);
    ++g.moves;
    set_status(app, "Moved tableau card to foundation.");
    return true;
}

bool tableau_to_tableau(App& app, size_t src, size_t dest, size_t count) {
    Game& g = app.game;
    if (src >= kTableauCount || dest >= kTableauCount || src == dest) {
        set_status(app, "Bad tableau move.");
        return false;
    }
    Pile& from = g.tableau[src];
    Pile& to = g.tableau[dest];
    if (count == 0 || count > from.count) {
        set_status(app, "No such run in source pile.");
        return false;
    }
    size_t first = from.count - count;
    for (size_t i = first; i < from.count; ++i) {
        if (!from.face_up[i]) {
            set_status(app, "That run includes a face-down card.");
            return false;
        }
    }
    for (size_t i = first + 1; i < from.count; ++i) {
        uint8_t upper = from.cards[i - 1];
        uint8_t lower = from.cards[i];
        if (is_red(upper) == is_red(lower) || rank(upper) != rank(lower) + 1) {
            set_status(app, "That is not a descending alternating run.");
            return false;
        }
    }
    if (!can_place_on_tableau(from.cards[first], to)) {
        set_status(app, "That run cannot go there.");
        return false;
    }
    save_undo(app);
    uint8_t moving[kMaxPile];
    for (size_t i = 0; i < count; ++i) {
        moving[i] = from.cards[first + i];
    }
    from.count = first;
    reveal_tail(from);
    for (size_t i = 0; i < count; ++i) {
        pile_push(to, moving[i], true);
    }
    ++g.moves;
    set_status(app, "Moved tableau run.");
    return true;
}

void clear_selection(App& app) {
    app.selected_place = MovePlace::Unknown;
    app.selected_pile = 0;
}

bool has_selection(const App& app) {
    return app.selected_place == MovePlace::Waste ||
           app.selected_place == MovePlace::Tableau;
}

void select_waste(App& app) {
    if (app.game.waste.count == 0) {
        clear_selection(app);
        set_status(app, "Waste is empty.");
        return;
    }
    app.selected_place = MovePlace::Waste;
    app.selected_pile = 0;
    set_status(app, "Selected waste. Press 1-7 or f.");
}

void select_tableau(App& app, size_t pile) {
    if (pile >= kTableauCount || app.game.tableau[pile].count == 0) {
        clear_selection(app);
        set_status(app, "That tableau pile is empty.");
        return;
    }
    app.selected_place = MovePlace::Tableau;
    app.selected_pile = pile;
    set_status(app, "Selected tableau pile. Press 1-7 or f.");
}

bool is_valid_tail_run(const Pile& pile, size_t first) {
    if (first >= pile.count) return false;
    for (size_t i = first; i < pile.count; ++i) {
        if (!pile.face_up[i]) return false;
    }
    for (size_t i = first + 1; i < pile.count; ++i) {
        uint8_t upper = pile.cards[i - 1];
        uint8_t lower = pile.cards[i];
        if (is_red(upper) == is_red(lower) || rank(upper) != rank(lower) + 1) {
            return false;
        }
    }
    return true;
}

size_t best_tableau_run_count(const Game& game, size_t src, size_t dest) {
    if (src >= kTableauCount || dest >= kTableauCount || src == dest) {
        return 0;
    }
    const Pile& from = game.tableau[src];
    const Pile& to = game.tableau[dest];
    for (size_t count = from.count; count > 0; --count) {
        size_t first = from.count - count;
        if (is_valid_tail_run(from, first) &&
            can_place_on_tableau(from.cards[first], to)) {
            return count;
        }
    }
    return 0;
}

bool move_selection_to_tableau(App& app, size_t dest) {
    if (!has_selection(app)) {
        select_tableau(app, dest);
        return true;
    }

    bool moved = false;
    if (app.selected_place == MovePlace::Waste) {
        moved = waste_to_tableau(app, dest);
    } else if (app.selected_place == MovePlace::Tableau) {
        if (app.selected_pile == dest) {
            set_status(app, "Selection cleared.");
        } else {
            size_t count = best_tableau_run_count(app.game, app.selected_pile, dest);
            if (count == 0) {
                set_status(app, "No legal run from that pile fits there.");
            } else {
                moved = tableau_to_tableau(app, app.selected_pile, dest, count);
            }
        }
    }
    clear_selection(app);
    return moved;
}

bool auto_foundation(App& app) {
    if (waste_to_foundation(app)) return true;
    for (size_t i = 0; i < kTableauCount; ++i) {
        if (tableau_to_foundation(app, i)) return true;
    }
    set_status(app, "No foundation move is available.");
    return false;
}

bool move_selection_to_foundation(App& app) {
    if (!has_selection(app)) {
        return auto_foundation(app);
    }

    bool moved = false;
    if (app.selected_place == MovePlace::Waste) {
        moved = waste_to_foundation(app);
    } else if (app.selected_place == MovePlace::Tableau) {
        moved = tableau_to_foundation(app, app.selected_pile);
    }
    clear_selection(app);
    return moved;
}

bool won(const Game& g) {
    for (size_t i = 0; i < kFoundationCount; ++i) {
        if (g.foundations[i].count != 13) return false;
    }
    return true;
}

const char* skip_spaces(const char* text) {
    while (*text == ' ' || *text == '\t') ++text;
    return text;
}

char lower_char(char ch) {
    if (ch >= 'A' && ch <= 'Z') {
        return static_cast<char>(ch - 'A' + 'a');
    }
    return ch;
}

struct Token {
    const char* text;
    size_t len;
};

bool next_token(const char*& text, Token& out) {
    text = skip_spaces(text);
    if (*text == '\0') return false;
    out.text = text;
    out.len = 0;
    while (text[out.len] != '\0' && text[out.len] != ' ' &&
           text[out.len] != '\t') {
        ++out.len;
    }
    text += out.len;
    return true;
}

bool token_equals(const Token& token, const char* word) {
    size_t i = 0;
    while (word[i] != '\0' && i < token.len) {
        if (lower_char(token.text[i]) != word[i]) return false;
        ++i;
    }
    return i == token.len && word[i] == '\0';
}

bool token_is_any(const Token& token, const char* a, const char* b) {
    return token_equals(token, a) || token_equals(token, b);
}

bool token_is_any(const Token& token,
                  const char* a,
                  const char* b,
                  const char* c) {
    return token_equals(token, a) || token_equals(token, b) ||
           token_equals(token, c);
}

bool token_to_pile(const Token& token, size_t& out) {
    if (token.len != 1 || token.text[0] < '1' || token.text[0] > '7') {
        return false;
    }
    out = static_cast<size_t>(token.text[0] - '1');
    return true;
}

bool token_to_count(const Token& token, size_t& out) {
    if (token.len == 0 || token.text[0] < '1' || token.text[0] > '9') {
        return false;
    }
    size_t value = 0;
    for (size_t i = 0; i < token.len; ++i) {
        char ch = token.text[i];
        if (ch < '0' || ch > '9') return false;
        value = value * 10 + static_cast<size_t>(ch - '0');
    }
    out = value;
    return true;
}

bool is_skip_word(const Token& token) {
    return token_is_any(token, "to", "onto", "on") ||
           token_is_any(token, "from", "pile", "tableau");
}

struct MovePart {
    MovePlace place;
    size_t pile;
};

bool parse_move_part(const Token& token, MovePart& part) {
    size_t pile = 0;
    if (token_to_pile(token, pile)) {
        part.place = MovePlace::Tableau;
        part.pile = pile;
        return true;
    }
    if (token_is_any(token, "w", "waste")) {
        part.place = MovePlace::Waste;
        part.pile = 0;
        return true;
    }
    if (token_is_any(token, "f", "foundation", "foundations")) {
        part.place = MovePlace::Foundation;
        part.pile = 0;
        return true;
    }
    return false;
}

bool execute_tableau_move(App& app,
                          size_t src,
                          size_t dest,
                          size_t count,
                          bool count_set) {
    if (!count_set) {
        count = best_tableau_run_count(app.game, src, dest);
        if (count == 0) {
            set_status(app, "No legal run from that pile fits there.");
            return false;
        }
    }
    return tableau_to_tableau(app, src, dest, count);
}

bool handle_wordy_move(App& app, const char* args) {
    MovePart parts[2]{{MovePlace::Unknown, 0}, {MovePlace::Unknown, 0}};
    size_t part_count = 0;
    size_t count = 0;
    bool count_set = false;
    bool saw_run = false;

    const char* p = args;
    Token token{};
    while (next_token(p, token)) {
        if (is_skip_word(token)) continue;
        if (token_is_any(token, "run", "cards", "card")) {
            saw_run = true;
            continue;
        }

        MovePart part{MovePlace::Unknown, 0};
        if (part_count < 2 && parse_move_part(token, part)) {
            parts[part_count++] = part;
            continue;
        }

        size_t parsed_count = 0;
        if (token_to_count(token, parsed_count)) {
            count = parsed_count;
            count_set = true;
            continue;
        }

        return false;
    }

    (void)saw_run;
    if (part_count == 2) {
        MovePart first = parts[0];
        MovePart second = parts[1];
        if (first.place == MovePlace::Waste &&
            second.place == MovePlace::Foundation) {
            return waste_to_foundation(app);
        }
        if (first.place == MovePlace::Foundation &&
            second.place == MovePlace::Waste) {
            return waste_to_foundation(app);
        }
        if (first.place == MovePlace::Waste &&
            second.place == MovePlace::Tableau) {
            return waste_to_tableau(app, second.pile);
        }
        if (first.place == MovePlace::Tableau &&
            second.place == MovePlace::Foundation) {
            return tableau_to_foundation(app, first.pile);
        }
        if (first.place == MovePlace::Tableau &&
            second.place == MovePlace::Tableau) {
            execute_tableau_move(app, first.pile, second.pile, count, count_set);
            return true;
        }
    }

    set_status(app, "Try: move waste foundation or move run 4 to 2");
    return false;
}

void handle_command(App& app, const char* cmd) {
    clear_selection(app);
    const char* p = skip_spaces(cmd);
    if (*p == '/' || *p == ':') {
        ++p;
        p = skip_spaces(p);
    }
    if (*p == '\0') return;
    Token command{};
    if (!next_token(p, command)) return;

    if (token_is_any(command, "d", "draw")) {
        app.show_help = false;
        draw_stock(app);
    } else if (token_is_any(command, "m", "move")) {
        app.show_help = false;
        handle_wordy_move(app, p);
    } else if (token_is_any(command, "f", "foundation", "auto")) {
        app.show_help = false;
        auto_foundation(app);
    } else if (token_is_any(command, "u", "undo")) {
        app.show_help = false;
        if (app.undo_count == 0) {
            set_status(app, "Nothing to undo.");
        } else {
            app.game = app.undo[--app.undo_count];
            set_status(app, "Undid one move.");
        }
    } else if (token_is_any(command, "r", "restart", "new")) {
        app.show_help = false;
        save_undo(app);
        deal(app.game, initial_seed());
        set_status(app, "New deal.");
    } else if (token_is_any(command, "q", "quit", "exit")) {
        app.quit = true;
    } else if (token_is_any(command, "?", "h", "help")) {
        app.show_help = !app.show_help;
        set_status(app, app.show_help ? "Help is open." : "Help closed.");
    } else {
        set_status(app, "Unknown command. Type help.");
    }
}

bool handle_quick_key(App& app, char ch) {
    if (ch >= 'A' && ch <= 'Z') {
        ch = static_cast<char>(ch - 'A' + 'a');
    }
    if (ch >= '1' && ch <= '7') {
        app.show_help = false;
        move_selection_to_tableau(app, static_cast<size_t>(ch - '1'));
        return true;
    }
    if (ch == 'w') {
        app.show_help = false;
        select_waste(app);
        return true;
    }
    if (ch == 'd') {
        app.show_help = false;
        clear_selection(app);
        draw_stock(app);
        return true;
    }
    if (ch == 'f' || ch == 'a') {
        app.show_help = false;
        move_selection_to_foundation(app);
        return true;
    }
    if (ch == 'u') {
        app.show_help = false;
        clear_selection(app);
        if (app.undo_count == 0) {
            set_status(app, "Nothing to undo.");
        } else {
            app.game = app.undo[--app.undo_count];
            set_status(app, "Undid one move.");
        }
        return true;
    }
    if (ch == 'r') {
        app.show_help = false;
        clear_selection(app);
        save_undo(app);
        deal(app.game, initial_seed());
        set_status(app, "New deal.");
        return true;
    }
    if (ch == '?' || ch == 'h') {
        clear_selection(app);
        app.show_help = !app.show_help;
        set_status(app, app.show_help ? "Help is open." : "Help closed.");
        return true;
    }
    if (ch == 'q') {
        app.quit = true;
        return true;
    }
    return false;
}

void draw_card(long console, const Pile& pile, size_t index) {
    if (index >= pile.count) {
        set_color(console, kDimFg, kDefaultBg);
        write_text(console, "     ");
        return;
    }
    if (!pile.face_up[index]) {
        set_color(console, kDimFg, kDefaultBg);
        write_text(console, "[###]");
        return;
    }
    uint8_t card = pile.cards[index];
    set_color(console, is_red(card) ? kRedFg : kBlackFg, kDefaultBg);
    write_char(console, '[');
    write_text(console, rank_text(rank(card)));
    if (rank(card) != 10) write_char(console, ' ');
    write_char(console, suit_text(suit(card)));
    write_char(console, ']');
}

void draw_top_card(long console, const Pile& pile) {
    if (pile.count == 0) {
        set_color(console, kDimFg, kDefaultBg);
        write_text(console, "[   ]");
        return;
    }
    draw_card(console, pile, pile.count - 1);
}

void draw_pile_label(long console, const char* text, bool selected) {
    set_color(console,
              selected ? kSelectFg : kDefaultFg,
              selected ? kSelectBg : kDefaultBg);
    write_text(console, text);
    set_color(console, kDefaultFg, kDefaultBg);
}

size_t max_tableau_height(const Game& g) {
    size_t height = 0;
    for (size_t i = 0; i < kTableauCount; ++i) {
        if (g.tableau[i].count > height) height = g.tableau[i].count;
    }
    return height;
}

void render(App& app, long console, const char* command, size_t command_len) {
    const Game& g = app.game;
    defer_updates(console, true);
    clear_console(console);
    set_color(console, kHeaderFg, kHeaderBg);
    write_text(console, " Solitaire ");
    set_color(console, kDefaultFg, kDefaultBg);
    write_text(console, "  help  draw  move  auto  undo  restart  quit\n\n");

    write_text(console, "Stock(d) ");
    set_color(console, g.stock.count ? kDimFg : kDefaultFg, kDefaultBg);
    write_text(console, g.stock.count ? "[###]" : "[   ]");
    set_color(console, kDefaultFg, kDefaultBg);
    write_text(console, "  ");
    draw_pile_label(console, "Waste(w)", app.selected_place == MovePlace::Waste);
    write_char(console, ' ');
    draw_top_card(console, g.waste);
    set_color(console, kDefaultFg, kDefaultBg);
    write_text(console, "     Foundations(f) ");
    for (size_t i = 0; i < kFoundationCount; ++i) {
        draw_top_card(console, g.foundations[i]);
        write_char(console, ' ');
    }
    write_text(console, "\n\n");

    for (size_t col = 0; col < kTableauCount; ++col) {
        char label[] = "  1   ";
        label[2] = static_cast<char>('1' + col);
        draw_pile_label(console,
                        label,
                        app.selected_place == MovePlace::Tableau &&
                            app.selected_pile == col);
        write_char(console, ' ');
    }
    write_char(console, '\n');

    size_t height = max_tableau_height(g);
    if (height < 12) height = 12;
    for (size_t row = 0; row < height; ++row) {
        for (size_t col = 0; col < kTableauCount; ++col) {
            draw_card(console, g.tableau[col], row);
            write_char(console, ' ');
            write_char(console, ' ');
        }
        write_char(console, '\n');
    }

    char line[160]{};
    append_text(line, sizeof(line), "\nMoves: ");
    append_uint(line, sizeof(line), g.moves);
    append_text(line, sizeof(line), "   Stock: ");
    append_uint(line, sizeof(line), static_cast<uint32_t>(g.stock.count));
    append_text(line, sizeof(line), "   Waste: ");
    append_uint(line, sizeof(line), static_cast<uint32_t>(g.waste.count));
    if (won(g)) append_text(line, sizeof(line), "   You won!");
    append_text(line, sizeof(line), "\n");
    set_color(console, kDefaultFg, kDefaultBg);
    write_text(console, line);

    set_color(console, kDimFg, kDefaultBg);
    write_text(console, app.status);
    if (app.show_help) {
        write_text(console, "\n\nHelp\n");
        write_text(console, "  Quick: d draw, w select waste, 1-7 select/move, f foundation\n");
        write_text(console, "  Quick: u undo, r restart, ?/h help, q quit, Esc clear\n");
        write_text(console, "  draw | d                  draw stock / recycle waste\n");
        write_text(console, "  auto | foundation | f     move one available card up\n");
        write_text(console, "  move waste 3 | m w 3      waste to tableau pile 3\n");
        write_text(console, "  move waste foundation     waste to foundation\n");
        write_text(console, "  move foundation waste     same as waste to foundation\n");
        write_text(console, "  move 4 foundation         tableau pile 4 to foundation\n");
        write_text(console, "  move run 4 to 2           longest legal run 4 -> 2\n");
        write_text(console, "  move 4 2 3 | m 4 2 3      exactly 3 cards 4 -> 2\n");
        write_text(console, "  undo | u, restart | r, quit | q\n");
    }
    write_text(console, "\n> ");
    set_color(console, kDefaultFg, kDefaultBg);
    if (command_len > 0) {
        descriptor_write(static_cast<uint32_t>(console), command, command_len);
    }
    defer_updates(console, false);
}

bool read_key(uint32_t keyboard, char& out) {
    descriptor_defs::KeyboardEvent events[8]{};
    while (true) {
        long read = descriptor_read(keyboard, events, sizeof(events));
        if (read <= 0) {
            yield();
            continue;
        }
        size_t count = static_cast<size_t>(read) / sizeof(events[0]);
        for (size_t i = 0; i < count; ++i) {
            const auto& ev = events[i];
            if (!keyboard::is_pressed(ev) || keyboard::is_extended(ev)) {
                continue;
            }
            char ch = keyboard::scancode_to_char(ev.scancode, ev.mods);
            if (ch != 0) {
                out = ch;
                return true;
            }
        }
    }
}

}  // namespace

int main() {
    long console = descriptor_open(kDescConsole, 0);
    long keyboard = descriptor_open(kDescKeyboard, 0);
    if (console < 0 || keyboard < 0) {
        return 1;
    }

    App& app = g_app;
    memset(&app, 0, sizeof(app));
    deal(app.game, initial_seed());
    set_status(app, "Ready. Press d, w, 1-7, or ? for help.");

    char command[kCommandSize]{};
    size_t command_len = 0;
    render(app, console, command, command_len);

    while (!app.quit) {
        char ch = 0;
        if (!read_key(static_cast<uint32_t>(keyboard), ch)) continue;
        if (ch == '\n' || ch == '\r') {
            command[command_len] = '\0';
            handle_command(app, command);
            command_len = 0;
            command[0] = '\0';
        } else if (ch == '\b' || ch == 0x7F) {
            if (command_len > 0) {
                command[--command_len] = '\0';
            }
        } else if (ch == 27) {
            command_len = 0;
            command[0] = '\0';
            clear_selection(app);
            set_status(app, "Cleared.");
        } else if (command_len == 0 && handle_quick_key(app, ch)) {
            command[0] = '\0';
        } else if (ch >= 0x20 && ch <= 0x7E && command_len + 1 < sizeof(command)) {
            command[command_len++] = ch;
            command[command_len] = '\0';
        }
        render(app, console, command, command_len);
    }

    clear_console(console);
    set_color(console, kDefaultFg, kDefaultBg);
    return 0;
}
