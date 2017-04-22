//
// ewig - an immutable text editor
// Copyright (C) 2017 Juan Pedro Bolivar Puente
//
// This file is part of ewig.
//
// ewig is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// ewig is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with ewig.  If not, see <http://www.gnu.org/licenses/>.
//

#include "ewig/tui.hpp"
#include "ewig/keys.hpp"

#include <immer/flex_vector_transient.hpp>
#include <immer/algorithm.hpp>
#include <boost/optional.hpp>

#include <ncursesw/ncurses.h>

#include <algorithm>
#include <fstream>
#include <iostream>
#include <string>
#include <functional>

using namespace std::placeholders;
using namespace std::string_literals;

namespace ewig {

namespace {

enum color
{
    message_color = 1,
    selection_color,
};

const auto key_map_emacs = make_key_map(
{
    {key::seq(key::up),        "move-up"},
    {key::seq(key::down),      "move-down"},
    {key::seq(key::left),      "move-left"},
    {key::seq(key::right),     "move-right"},
    {key::seq(key::page_down), "page-down"},
    {key::seq(key::page_up),   "page-up"},
    {key::seq(key::backspace), "delete-char"},
    {key::seq(key::delete_),   "delete-char-right"},
    {key::seq(key::home),      "move-beginning-of-line"},
    {key::seq(key::ctrl('A')), "move-beginning-of-line"},
    {key::seq(key::end),       "move-end-of-line"},
    {key::seq(key::ctrl('E')), "move-end-of-line"},
    {key::seq(key::ctrl('I')), "insert-tab"}, // tab
    {key::seq(key::ctrl('J')), "new-line"}, // enter
    {key::seq(key::ctrl('K')), "kill-line"},
    {key::seq(key::ctrl('W')), "cut"},
    {key::seq(key::ctrl('Y')), "paste"},
    {key::seq(key::ctrl('@')), "start-selection"}, // ctrl-space
    {key::seq(key::ctrl('X'), key::ctrl('C')), "quit"},
    {key::seq(key::alt('w')),  "copy"},
});

coord get_editor_size()
{
    int maxrow, maxcol;
    getmaxyx(stdscr, maxrow, maxcol);
    // make space for minibuffer and modeline
    return {maxrow - 2, maxcol};
}

// Fills the string `str` with the display contents of the line `ln`
// between the display columns `first_col` and `first_col + num_col`.
// It takes into account tabs, expanding them correctly.
void display_line_fill(const line& ln, int first_col, int num_col,
                       std::wstring& str)
{
    using namespace std;
    auto cur_col = index{};
    for (auto c : ln) {
        if (num_col == (index)str.size())
            return;
        else if (c == '\t') {
            auto next_col = cur_col + tab_width - (cur_col % tab_width);
            auto to_fill  = std::min(next_col, first_col + num_col) -
                            std::max(cur_col, first_col);
            std::fill_n(back_inserter(str), to_fill, ' ');
            cur_col = next_col;
        } else if (cur_col >= first_col) {
            str.push_back(c);
            ++cur_col;
        } else
            ++cur_col;
    }
}

void draw_text(file_buffer buf, coord size)
{
    using namespace std;
    attrset(A_NORMAL);
    int col, row;
    getyx(stdscr, col, row);
    auto str      = std::wstring{};
    auto first_ln = begin(buf.content) +
        min(buf.scroll.row, (index)buf.content.size());
    auto last_ln  = begin(buf.content) +
        min(size.row + buf.scroll.row, (index)buf.content.size());

    coord starts, ends;
    auto has_selection = bool(buf.selection_start);
    if (has_selection) {
        std::tie(starts, ends) = selected_region(buf);
        starts.row -= buf.scroll.row;
        ends.row   -= buf.scroll.row;
    }

    immer::for_each(first_ln, last_ln, [&] (auto ln) {
        str.clear();
        display_line_fill(ln, buf.scroll.col + col, size.col, str);
        move(row, col);
        if (has_selection) {
            if (starts.row == ends.row && starts.row == row) {
                addnwstr(str.c_str(), starts.col);
                attron(COLOR_PAIR(selection_color));
                addnwstr(str.c_str() + starts.col, ends.col - starts.col);
                attroff(COLOR_PAIR(selection_color));
                addnwstr(str.c_str() + ends.col, str.size() - ends.col);
            } else if (starts.row == row) {
                addnwstr(str.c_str(), starts.col);
                attron(COLOR_PAIR(selection_color));
                addnwstr(str.c_str() + starts.col, str.size() - starts.col);
                hline(' ', size.col);
                attroff(COLOR_PAIR(selection_color));
            } else if (ends.row == row) {
                attron(COLOR_PAIR(selection_color));
                addnwstr(str.c_str(), ends.col);
                attroff(COLOR_PAIR(selection_color));
                addnwstr(str.c_str() + ends.col, str.size() - ends.col);
            } else if (starts.row < row && ends.row > row) {
                attron(COLOR_PAIR(selection_color));
                addwstr(str.c_str());
                hline(' ', size.col);
                attroff(COLOR_PAIR(selection_color));
            } else {
                addwstr(str.c_str());
            }
        } else {
            addwstr(str.c_str());
        }
        row++;
    });
}

void draw_mode_line(const file_buffer& buffer, int maxcol)
{
    attrset(A_REVERSE);
    auto dirty_mark = buffer.content == buffer.file_content ? "--" : "**";
    printw(" %s %s  (%d, %d)",
           dirty_mark,
           buffer.file_name.get().c_str(),
           buffer.cursor.col,
           buffer.cursor.row);
    hline(' ', maxcol);
}

void draw_message(const message& msg)
{
    attrset(A_NORMAL);
    attron(COLOR_PAIR(message_color));
    addstr("message: ");
    addstr(msg.content.get().c_str());
    attroff(COLOR_PAIR(message_color));
}

void draw_text_cursor(const file_buffer& buf, coord window_size)
{
    auto cursor = actual_display_cursor(buf);
    move(cursor.row - buf.scroll.row, cursor.col - buf.scroll.col);
    curs_set(cursor.col >= buf.scroll.col &&
             cursor.row >= buf.scroll.row &&
             cursor.col < buf.scroll.col + window_size.col &&
             cursor.row < buf.scroll.row + window_size.row);
}

void draw(const app_state& app)
{
    auto size = get_editor_size();
    erase();

    move(0, 0);
    draw_text(app.buffer, size);

    move(size.row, 0);
    draw_mode_line(app.buffer, size.col);

    if (!app.messages.empty()) {
        move(size.row + 1, 0);
        draw_message(app.messages.back());
    }

    draw_text_cursor(app.buffer, size);
    refresh();
}

class key_handler
{
    key_map map_;
    key_seq seq_;

public:
    key_handler(key_map map)
        : map_{map}
        , seq_{}
    {}

    boost::optional<app_state> handle_key(app_state state, int res, wchar_t key)
    {
        seq_.push_back({res, key});
        auto it = map_.find(seq_);
        if (it != map_.end()) {
            if (!it->second.empty()) {
                seq_.clear();
                return eval_command(state, it->second, get_editor_size());
            } else {
                return state;
            }
        } else {
            auto is_single_char = seq_.size() == 1;
            seq_.clear();
            if (is_single_char && res == OK && !std::iscntrl(key)) {
                state = put_message(state, "adding character: "s + key_name(key));
                return eval_insert_char(state, key, get_editor_size());
            } else {
                return put_message(state, "unbound key sequence");
            }
        }
    }
};

} // anonymous namespace

tui::tui(const char* file_name)
    : state{load_file(file_name)}
{
    initscr();
    raw();
    noecho();
    keypad(stdscr, true);
    start_color();
    use_default_colors();
    init_pair(message_color,   COLOR_YELLOW, -1);
    init_pair(selection_color, COLOR_BLACK, COLOR_YELLOW);
    draw(state);
}

tui::~tui()
{
    endwin();
}

int tui::run()
{
    auto input = key_handler{key_map_emacs};
    auto key = wint_t{};
    auto res = get_wch(&key);
    while (auto new_state = input.handle_key(state, res, key)) {
        state = *new_state;
        draw(state);
        res = get_wch(&key);
    }
    return 0;
}

int main(int argc, char* argv[])
{
    std::locale::global(std::locale(""));

    if (argc != 2) {
        std::cerr << "Give me a file name." << std::endl;
        return 1;
    }

    tui editor{argv[1]};
    editor.run();

    return 0;
}

} // namespace ewig
