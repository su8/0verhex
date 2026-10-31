/*
 * Copyright 10/31/2026 12/05/2025 https://github.com/su8/0verhex
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA 02110-1301, USA.
 *
 * Compile with: g++ -std=c++11 -Wall -Wextra -O2 -o 0verhex main.cpp -lncurses
 */

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>
#include <ncurses.h>

// ---------------------------------------------------------------------------
// The file in memory, with an undo history that knows about all the changes
// ---------------------------------------------------------------------------

// One change: the bytes at offset were replaced by other bytes. An overwrite has the same number of
// bytes before and after, an insert has no bytes before, a delete has no bytes after.
struct Edit {
  size_t offset;
  std::vector<unsigned char> before;
  std::vector<unsigned char> after;
  std::vector<bool> flagsBefore;   // were the old bytes changed already
};

struct Document {
  std::vector<unsigned char> data;
  std::vector<bool> changed;       // changed since the file was loaded or saved, drawn in red
  std::vector<Edit> undoStack;
  std::vector<Edit> redoStack;
  uint32_t sum;                    // the sum of all bytes, kept up to date by every change
  size_t cleanDepth;               // undoStack.size() when the file was saved, SIZE_MAX: never again
  Document() : sum(0), cleanDepth(0) {}
  bool dirty() const { return undoStack.size() != cleanDepth; }
  uint8_t checkSum() const { return static_cast<uint8_t>(sum & 0xFFU); } // sum of the bytes modulo 256
};

// Replace removeLen bytes at offset with the bytes in insert. Every change goes through here
static void splice(Document &doc, size_t offset, size_t removeLen, const std::vector<unsigned char> &insert, const std::vector<bool> &insertFlags) {
  const std::ptrdiff_t at = static_cast<std::ptrdiff_t>(offset);
  const std::ptrdiff_t end = static_cast<std::ptrdiff_t>(offset + removeLen);
  for (size_t i = 0; i < removeLen; i++) { doc.sum -= doc.data[offset + i]; }
  for (size_t i = 0; i < insert.size(); i++) { doc.sum += insert[i]; }
  doc.data.erase(doc.data.begin() + at, doc.data.begin() + end);
  doc.changed.erase(doc.changed.begin() + at, doc.changed.begin() + end);
  doc.data.insert(doc.data.begin() + at, insert.begin(), insert.end());
  doc.changed.insert(doc.changed.begin() + at, insertFlags.begin(), insertFlags.end());
}

// A new change, that can be undone
static bool doEdit(Document &doc, size_t offset, size_t removeLen, const std::vector<unsigned char> &insert) {
  if (offset > doc.data.size() || removeLen > doc.data.size() - offset || (removeLen == 0 && insert.empty())) { return false; }
  Edit e;
  e.offset = offset;
  e.before.assign(doc.data.begin() + static_cast<std::ptrdiff_t>(offset), doc.data.begin() + static_cast<std::ptrdiff_t>(offset + removeLen));
  e.flagsBefore.assign(doc.changed.begin() + static_cast<std::ptrdiff_t>(offset), doc.changed.begin() + static_cast<std::ptrdiff_t>(offset + removeLen));
  e.after = insert;
  // after an undo past the saved state the saved state is lost with the redo history
  if (doc.undoStack.size() < doc.cleanDepth) { doc.cleanDepth = SIZE_MAX; }
  splice(doc, offset, removeLen, insert, std::vector<bool>(insert.size(), true));
  doc.undoStack.push_back(e);
  doc.redoStack.clear();
  return true;
}

static bool undo(Document &doc, size_t &offset) {
  if (doc.undoStack.empty()) { return false; }
  Edit e = doc.undoStack.back();
  doc.undoStack.pop_back();
  splice(doc, e.offset, e.after.size(), e.before, e.flagsBefore);
  offset = e.offset;
  doc.redoStack.push_back(e);
  return true;
}

static bool redo(Document &doc, size_t &offset) {
  if (doc.redoStack.empty()) { return false; }
  Edit e = doc.redoStack.back();
  doc.redoStack.pop_back();
  splice(doc, e.offset, e.before.size(), e.after, std::vector<bool>(e.after.size(), true));
  offset = e.offset;
  doc.undoStack.push_back(e);
  return true;
}

// ---------------------------------------------------------------------------
// File I/O
// ---------------------------------------------------------------------------

static bool loadFile(const std::string &filename, Document &doc, std::string &error) {
  struct stat st;
  if (stat(filename.c_str(), &st) != 0) { error = std::strerror(errno); return false; }
  if (!S_ISREG(st.st_mode)) { error = "not a regular file"; return false; }
  FILE *f = std::fopen(filename.c_str(), "rb");
  if (!f) { error = std::strerror(errno); return false; }
  std::vector<unsigned char> data;
  unsigned char chunk[65536];
  size_t n;
  while ((n = std::fread(chunk, 1, sizeof(chunk), f)) > 0) { data.insert(data.end(), chunk, chunk + n); }
  const bool failed = std::ferror(f) != 0;
  if (failed) { error = std::strerror(errno); }
  std::fclose(f);
  if (failed) { return false; }
  doc.data.swap(data);
  doc.changed.assign(doc.data.size(), false);
  doc.sum = 0;
  for (size_t i = 0; i < doc.data.size(); i++) { doc.sum += doc.data[i]; }
  doc.undoStack.clear();
  doc.redoStack.clear();
  doc.cleanDepth = 0;
  return true;
}

// Write a temporary file next to the original and rename it over the original. The old file is
// kept when anything fails, a full disk can't leave a half written file behind
static bool saveFile(const std::string &filename, Document &doc, std::string &error) {
  char *resolved = realpath(filename.c_str(), NULL); // a symbolic link stays a link
  const std::string target = resolved ? resolved : filename;
  std::free(resolved);
  const std::string temp = target + ".0verhex.tmp";
  struct stat st;
  const bool haveMode = stat(target.c_str(), &st) == 0;
  FILE *f = std::fopen(temp.c_str(), "wb");
  if (!f) { error = std::strerror(errno); return false; }
  int err = 0;
  if (!doc.data.empty() && std::fwrite(doc.data.data(), 1, doc.data.size(), f) != doc.data.size()) { err = errno ? errno : EIO; }
  if (!err && std::fflush(f) != 0) { err = errno; }
  if (!err && fsync(fileno(f)) != 0) { err = errno; }
  if (std::fclose(f) != 0 && !err) { err = errno; }
  if (!err && haveMode) { if (chmod(temp.c_str(), st.st_mode & 07777) != 0) { /* the new file keeps the default mode */ } }
  if (!err && std::rename(temp.c_str(), target.c_str()) != 0) { err = errno; }
  if (err) {
    error = std::strerror(err);
    std::remove(temp.c_str());
    return false;
  }
  doc.cleanDepth = doc.undoStack.size();
  std::fill(doc.changed.begin(), doc.changed.end(), false);
  return true;
}

// ---------------------------------------------------------------------------
// Parsing what the user types
// ---------------------------------------------------------------------------

static bool isSeparator(char c) { return c == ' ' || c == '\t' || c == ','; }

static bool isHexDigit(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

static unsigned int hexValue(char c) {
  if (c >= '0' && c <= '9') { return static_cast<unsigned int>(c - '0'); }
  if (c >= 'a' && c <= 'f') { return static_cast<unsigned int>(c - 'a' + 10); }
  return static_cast<unsigned int>(c - 'A' + 10);
}

// "41 42 0x43 5 4a4b4c" -> 41 42 43 05 4A 4B 4C. A word is one or two hex digits, or an even number of
// them (a pasted string). bad gets the first word that is not valid
static bool parseHexBytes(const std::string &text, std::vector<unsigned char> &out, std::string &bad) {
  out.clear();
  size_t i = 0;
  while (i < text.size()) {
    while (i < text.size() && isSeparator(text[i])) { i++; }
    size_t j = i;
    while (j < text.size() && !isSeparator(text[j])) { j++; }
    if (j == i) { break; }
    const std::string word = text.substr(i, j - i);
    i = j;
    std::string digits = word;
    if (digits.size() >= 2 && digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X')) { digits = digits.substr(2); }
    bool valid = !digits.empty() && (digits.size() <= 2 || digits.size() % 2 == 0);
    for (size_t k = 0; valid && k < digits.size(); k++) { valid = isHexDigit(digits[k]); }
    if (!valid) { bad = word; out.clear(); return false; }
    if (digits.size() <= 2) {
      unsigned int v = 0;
      for (size_t k = 0; k < digits.size(); k++) { v = v * 16U + hexValue(digits[k]); }
      out.push_back(static_cast<unsigned char>(v));
    } else {
      for (size_t k = 0; k < digits.size(); k += 2) { out.push_back(static_cast<unsigned char>(hexValue(digits[k]) * 16U + hexValue(digits[k + 1]))); }
    }
  }
  return true;
}

// "1F40" or "0x1f40" -> 8000
static bool parseOffset(const std::string &text, size_t &value) {
  size_t a = 0, b = text.size();
  while (a < b && isSeparator(text[a])) { a++; }
  while (b > a && isSeparator(text[b - 1])) { b--; }
  std::string s = text.substr(a, b - a);
  if (s.size() >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { s = s.substr(2); }
  if (s.empty() || s.size() > sizeof(size_t) * 2) { return false; }
  size_t v = 0;
  for (size_t k = 0; k < s.size(); k++) {
    if (!isHexDigit(s[k])) { return false; }
    v = v * 16U + hexValue(s[k]);
  }
  value = v;
  return true;
}

// ---------------------------------------------------------------------------
// Searching
// ---------------------------------------------------------------------------

// The first match that starts at from or later (forward), or the last one that starts before from
// (backward). When there is none the search goes on at the other end of the file, wrapped says so.
// SIZE_MAX: no match
static size_t findBytes(const std::vector<unsigned char> &data, const std::vector<unsigned char> &pat, size_t from, bool forward, bool &wrapped) {
  typedef std::vector<unsigned char>::const_iterator Iter;
  wrapped = false;
  if (pat.empty() || pat.size() > data.size()) { return SIZE_MAX; }
  from = std::min(from, data.size());
  const size_t last = data.size() - pat.size(); // the last place where a match can start
  if (forward) {
    if (from <= last) {
      Iter it = std::search(data.begin() + static_cast<std::ptrdiff_t>(from), data.end(), pat.begin(), pat.end());
      if (it != data.end()) { return static_cast<size_t>(it - data.begin()); }
    }
    const Iter stop = data.begin() + static_cast<std::ptrdiff_t>(std::min(from + pat.size() - 1, data.size()));
    Iter it = std::search(data.begin(), stop, pat.begin(), pat.end());
    if (it != stop) { wrapped = true; return static_cast<size_t>(it - data.begin()); }
    return SIZE_MAX;
  }
  if (from > 0) {
    const Iter stop = data.begin() + static_cast<std::ptrdiff_t>(std::min(from + pat.size() - 1, data.size()));
    Iter it = std::find_end(data.begin(), stop, pat.begin(), pat.end());
    if (it != stop) { return static_cast<size_t>(it - data.begin()); }
  }
  if (from <= last) {
    Iter it = std::find_end(data.begin() + static_cast<std::ptrdiff_t>(from), data.end(), pat.begin(), pat.end());
    if (it != data.end()) { wrapped = true; return static_cast<size_t>(it - data.begin()); }
  }
  return SIZE_MAX;
}

// ---------------------------------------------------------------------------
// The screen
// ---------------------------------------------------------------------------

static volatile std::sig_atomic_t gTerminate = 0;

static void onSignal(int) { gTerminate = 1; }

struct Ui {
  WINDOW *hex;
  WINDOW *status;
  int rows, cols;
  bool tooSmall;
  size_t cursor;
  size_t start;                    // the offset of the first line on the screen, always a multiple of bpl
  size_t bpl;                      // bytes per line
  size_t addrDigits;
  std::string message;             // shown in the status line until the next key
  std::vector<unsigned char> lastPattern;
  Ui() : hex(NULL), status(NULL), rows(0), cols(0), tooSmall(false), cursor(0), start(0), bpl(16), addrDigits(8) {}
};

// 16 bytes per line, fewer when the terminal is narrow
static size_t chooseBytesPerLine(int cols, size_t addrDigits) {
  static const size_t widths[] = { 16, 8, 4, 2, 1 };
  for (size_t i = 0; i < sizeof(widths) / sizeof(widths[0]); i++) {
    // address, two spaces, 3 columns for every byte, a space, and the characters
    if (addrDigits + 2 + widths[i] * 3 + 1 + widths[i] <= static_cast<size_t>(cols)) { return widths[i]; }
  }
  return 1;
}

// The windows get the size of the terminal: at the start and after every resize
static void layoutWindows(Ui &ui, const Document &doc) {
  getmaxyx(stdscr, ui.rows, ui.cols);
  ui.tooSmall = ui.rows < 3 || ui.cols < 20;
  ui.addrDigits = doc.data.size() > 0xFFFFFFFFULL ? 12 : 8;
  ui.bpl = chooseBytesPerLine(ui.cols, ui.addrDigits);
  if (ui.tooSmall) { return; }
  if (!ui.hex) {
    ui.hex = newwin(ui.rows - 1, ui.cols, 0, 0);
    ui.status = newwin(1, ui.cols, ui.rows - 1, 0);
    keypad(ui.status, TRUE);
    wbkgd(ui.status, has_colors() ? static_cast<chtype>(COLOR_PAIR(5) | A_BOLD) : static_cast<chtype>(A_REVERSE));
  } else {
    wresize(ui.hex, ui.rows - 1, ui.cols);
    wresize(ui.status, 1, ui.cols);
    mvwin(ui.status, ui.rows - 1, 0);
  }
  clearok(stdscr, TRUE);
}

// Scroll so that the cursor is on the screen, and not further than the last line
static void ensureVisible(Ui &ui, const Document &doc) {
  if (ui.tooSmall) { return; }
  const size_t visible = static_cast<size_t>(std::max(1, getmaxy(ui.hex)));
  const size_t cursorRow = ui.cursor / ui.bpl;
  size_t startRow = ui.start / ui.bpl;
  if (cursorRow < startRow) { startRow = cursorRow; }
  else if (cursorRow >= startRow + visible) { startRow = cursorRow - visible + 1; }
  const size_t lastRow = doc.data.empty() ? 0 : (doc.data.size() - 1) / ui.bpl;
  const size_t maxStartRow = lastRow + 1 > visible ? lastRow + 1 - visible : 0;
  if (startRow > maxStartRow) { startRow = maxStartRow; }
  ui.start = startRow * ui.bpl;
}

// Offset, hex bytes and characters. The byte under the cursor is inverted in both columns, bytes that
// were changed are red
static void drawHexView(WINDOW *win, const Document &doc, const Ui &ui) {
  werase(win);
  const int rows = getmaxy(win);
  const int hexX = static_cast<int>(ui.addrDigits) + 2;
  const int asciiX = hexX + static_cast<int>(ui.bpl) * 3 + 1;
  for (int row = 0; row < rows; ++row) {
    const size_t offset = ui.start + static_cast<size_t>(row) * ui.bpl;
    if (offset >= doc.data.size()) { break; }
    char addr[32];
    std::snprintf(addr, sizeof(addr), "%0*zx", static_cast<int>(ui.addrDigits), offset);
    wattron(win, COLOR_PAIR(1) | A_BOLD);
    mvwaddstr(win, row, 0, addr);
    wattroff(win, COLOR_PAIR(1) | A_BOLD);
    for (size_t i = 0; i < ui.bpl && offset + i < doc.data.size(); ++i) {
      const size_t idx = offset + i;
      const unsigned char c = doc.data[idx];
      const bool changed = doc.changed[idx];
      const int cursorAttr = idx == ui.cursor ? A_REVERSE : 0;
      char hexText[4];
      std::snprintf(hexText, sizeof(hexText), "%02X", c);
      const int hexAttr = COLOR_PAIR(changed ? 6 : 2) | A_BOLD | cursorAttr;
      wattron(win, hexAttr);
      mvwaddstr(win, row, hexX + static_cast<int>(i) * 3, hexText);
      wattroff(win, hexAttr);
      const bool printable = c >= 0x20 && c < 0x7F; // not isprint(): the locale could make 0x80.. printable
      const int charAttr = COLOR_PAIR(changed ? 6 : (printable ? 3 : 4)) | A_BOLD | cursorAttr;
      wattron(win, charAttr);
      mvwaddch(win, row, asciiX + static_cast<int>(i), printable ? c : '.');
      wattroff(win, charAttr);
    }
  }
  wrefresh(win);
}

// [+] changed | file | offset/size (percent) | the byte | size | sum. A message replaces it until the next key
static void drawStatus(WINDOW *status, const Document &doc, const std::string &filename, const Ui &ui) {
  werase(status);
  char line[1024];
  if (!ui.message.empty()) {
    std::snprintf(line, sizeof(line), " %s", ui.message.c_str());
  } else {
    char byteText[64] = "";
    size_t percent = 0;
    if (!doc.data.empty()) {
      const unsigned char c = doc.data[ui.cursor];
      std::snprintf(byteText, sizeof(byteText), " | 0x%02X %u '%c'", c, static_cast<unsigned int>(c), (c >= 0x20 && c < 0x7F) ? c : '.');
      percent = static_cast<size_t>((static_cast<unsigned long long>(ui.cursor) + 1ULL) * 100ULL / doc.data.size());
    }
    std::snprintf(line, sizeof(line), " %s%s | 0x%zx/0x%zx (%zu%%)%s | %zu bytes | 8bitSum %u | ? help",
      doc.dirty() ? "[+] " : "", filename.c_str(), ui.cursor, doc.data.size(), percent, byteText, doc.data.size(), static_cast<unsigned int>(doc.checkSum()));
  }
  mvwaddnstr(status, 0, 0, line, ui.cols);
  wrefresh(status);
}

// A line of text in the status line. Enter accepts, Escape (or Ctrl+C) cancels
static bool promptLine(Ui &ui, const std::string &msg, std::string &out) {
  out.clear();
  curs_set(1);
  bool accepted = false;
  int errors = 0;
  for (;;) {
    werase(ui.status);
    // when the text is longer than the line the end of it is shown
    const size_t room = ui.cols > 1 ? static_cast<size_t>(ui.cols) - 1 : 1;
    std::string shown = msg + out;
    if (shown.size() > room) { shown = shown.substr(shown.size() - room); }
    mvwaddstr(ui.status, 0, 0, shown.c_str());
    wmove(ui.status, 0, static_cast<int>(shown.size()));
    wrefresh(ui.status);
    const int ch = wgetch(ui.status);
    if (gTerminate) { break; }
    if (ch == ERR) { if (++errors > 100) { break; } continue; } // the terminal is gone: don't wait for ever
    errors = 0;
    if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) { accepted = true; break; }
    if (ch == 27 || ch == 3) { break; }
    if ((ch == KEY_BACKSPACE || ch == 127 || ch == 8) && !out.empty()) { out.erase(out.size() - 1); }
    else if (ch >= 32 && ch < 127 && out.size() < 255) { out += static_cast<char>(ch); }
  }
  curs_set(0);
  return accepted;
}

static void showHelp(Ui &ui) {
  static const char *lines[] = {
    "Arrows, PageUp, PageDown, Home, End   move",
    "g    go to an offset (hex)",
    "e    overwrite bytes from the cursor      (hex: 41 42 43, or 414243)",
    "i    insert bytes before the cursor",
    "a    insert bytes after the cursor",
    "d    delete the byte under the cursor",
    "u    undo, r redo (they undo inserts and deletes too)",
    "/    search ASCII text    h search hex bytes",
    "n    next match, N previous match (they go around the file)",
    "s    save (the old file is kept if writing fails)",
    "q    quit, also Ctrl+C",
    "",
    "Changed bytes are red. [+] in the status line: not saved.",
    "Press any key",
  };
  werase(ui.hex);
  for (size_t i = 0; i < sizeof(lines) / sizeof(lines[0]) && static_cast<int>(i) < getmaxy(ui.hex); i++) { mvwaddnstr(ui.hex, static_cast<int>(i), 1, lines[i], ui.cols - 2); }
  wrefresh(ui.hex);
  wgetch(ui.status);
}

// ---------------------------------------------------------------------------
// The keys
// ---------------------------------------------------------------------------

static void moveCursorTo(Ui &ui, const Document &doc, size_t offset) {
  ui.cursor = doc.data.empty() ? 0 : std::min(offset, doc.data.size() - 1);
}

static void hexMessage(Ui &ui, const char *what, size_t offset) {
  char text[128];
  std::snprintf(text, sizeof(text), "%s at 0x%zx", what, offset);
  ui.message = text;
}

// Search for pattern from the cursor (forward) or before it (backward)
static void searchFrom(Ui &ui, const Document &doc, const std::vector<unsigned char> &pattern, bool forward) {
  bool wrapped = false;
  const size_t pos = findBytes(doc.data, pattern, forward ? ui.cursor + 1 : ui.cursor, forward, wrapped);
  if (pos == SIZE_MAX) { ui.message = "Not found"; return; }
  ui.cursor = pos;
  hexMessage(ui, wrapped ? "Found, the search went around the file" : "Found", pos);
}

// 'e', 'i' and 'a': ask for bytes and change the file
static void editBytes(Ui &ui, Document &doc, int key) {
  const char *msg = key == 'e' ? "Overwrite from the cursor, hex bytes (e.g. 41 42 43): "
                  : key == 'i' ? "Insert before the cursor, hex bytes (e.g. 41 42 43): "
                  : "Insert after the cursor, hex bytes (e.g. 41 42 43): ";
  if (key == 'e' && doc.data.empty()) { ui.message = "The file is empty: use i to insert bytes"; return; }
  std::string text, bad;
  std::vector<unsigned char> bytes;
  if (!promptLine(ui, msg, text)) { return; }
  if (!parseHexBytes(text, bytes, bad)) { ui.message = "Not a hex byte: " + bad; return; }
  if (bytes.empty()) { return; }
  if (key == 'e') {
    if (bytes.size() > doc.data.size() - ui.cursor) { ui.message = "Only " + std::to_string(doc.data.size() - ui.cursor) + " bytes are left after the cursor"; return; }
    doEdit(doc, ui.cursor, bytes.size(), bytes);
  } else {
    const size_t at = (key == 'i' || doc.data.empty()) ? ui.cursor : ui.cursor + 1;
    doEdit(doc, at, 0, bytes);
    ui.cursor = at;
  }
}

// Returns false when the program should end
static bool handleKey(Ui &ui, Document &doc, const std::string &filename, int ch) {
  const size_t size = doc.data.size();
  const size_t lastRow = size ? (size - 1) / ui.bpl : 0;
  const size_t row = ui.cursor / ui.bpl, col = ui.cursor % ui.bpl;
  const size_t page = static_cast<size_t>(std::max(1, getmaxy(ui.hex)));
  std::string text, bad;
  std::vector<unsigned char> pattern;
  size_t offset = 0;
  switch (ch) {
    case KEY_UP: if (ui.cursor >= ui.bpl) { ui.cursor -= ui.bpl; } break;
    case KEY_DOWN: if (row < lastRow) { ui.cursor = std::min(ui.cursor + ui.bpl, size - 1); } break; // the last short line too
    case KEY_LEFT: if (ui.cursor > 0) { ui.cursor--; } break;
    case KEY_RIGHT: if (ui.cursor + 1 < size) { ui.cursor++; } break;
    case KEY_PPAGE: ui.cursor = (row > page ? row - page : 0) * ui.bpl + col; break;
    case KEY_NPAGE: if (size) { ui.cursor = std::min(std::min(row + page, lastRow) * ui.bpl + col, size - 1); } break;
    case KEY_HOME: ui.cursor = row * ui.bpl; break;
    case KEY_END: if (size) { ui.cursor = std::min(row * ui.bpl + ui.bpl - 1, size - 1); } break;
    case 'g':
      if (promptLine(ui, "Go to offset (hex): ", text)) {
        if (!parseOffset(text, offset)) { ui.message = "Not a hex offset"; break; }
        if (offset >= size && size > 0) { ui.message = "Past the end, the last byte is shown"; }
        moveCursorTo(ui, doc, offset);
      }
      break;
    case 'u':
      if (undo(doc, offset)) { moveCursorTo(ui, doc, offset); ui.message = "Undone"; } else { ui.message = "Nothing to undo"; }
      break;
    case 'r':
      if (redo(doc, offset)) { moveCursorTo(ui, doc, offset); ui.message = "Redone"; } else { ui.message = "Nothing to redo"; }
      break;
    case 'e': case 'i': case 'a':
      editBytes(ui, doc, ch);
      break;
    case 'd':
      if (size && doEdit(doc, ui.cursor, 1, std::vector<unsigned char>())) { moveCursorTo(ui, doc, ui.cursor); }
      break;
    case '/':
      if (promptLine(ui, "Enter ASCII text to search: ", text) && !text.empty()) {
        ui.lastPattern.assign(text.begin(), text.end());
        searchFrom(ui, doc, ui.lastPattern, true);
      }
      break;
    case 'h':
      if (promptLine(ui, "Enter hex sequence (e.g., 48 65 6C): ", text)) {
        if (!parseHexBytes(text, pattern, bad)) { ui.message = "Not a hex byte: " + bad; break; }
        if (pattern.empty()) { break; }
        ui.lastPattern = pattern;
        searchFrom(ui, doc, ui.lastPattern, true);
      }
      break;
    case 'n': case 'N':
      if (ui.lastPattern.empty()) { ui.message = "Search first with / or h"; } else { searchFrom(ui, doc, ui.lastPattern, ch == 'n'); }
      break;
    case 's':
      if (saveFile(filename, doc, text)) { ui.message = "File saved"; } else { ui.message = "Error saving the file: " + text + " (the old file is untouched)"; }
      break;
    case '?':
      showHelp(ui);
      break;
    case 'q': case 3: // 3 is Ctrl+C: the terminal is in raw mode, it is not a signal
      if (!doc.dirty()) { return false; }
      if (promptLine(ui, "Unsaved changes! Type 'yes' to quit: ", text) && text == "yes") { return false; }
      ui.message = "Not quitting";
      break;
    default:
      break;
  }
  return true;
}

int main(int argc, char *argv[]) {
  if (argc < 2) { std::cerr << "You must provide some file to work on." << std::endl; return EXIT_FAILURE; }
  const std::string filename = argv[1];
  Document doc;
  std::string error;
  if (!loadFile(filename, doc, error)) { std::cerr << "Error: Cannot open file " << filename << ": " << error << std::endl; return EXIT_FAILURE; }

  // SIGTERM and SIGHUP end the program at once, without a question
  struct sigaction sa;
  std::memset(&sa, 0, sizeof(sa));
  sa.sa_handler = onSignal;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGTERM, &sa, NULL);
  sigaction(SIGHUP, &sa, NULL);
  sigaction(SIGINT, &sa, NULL);

  initscr();
  set_escdelay(25); // Escape cancels a prompt at once
  if (has_colors()) {
    start_color();
    init_pair(1, COLOR_CYAN, COLOR_BLACK);   // Offset
    init_pair(2, COLOR_YELLOW, COLOR_BLACK); // Hex
    init_pair(3, COLOR_GREEN, COLOR_BLACK);  // ASCII printable
    init_pair(4, COLOR_WHITE, COLOR_BLACK);  // ASCII non-printable
    init_pair(5, COLOR_WHITE, COLOR_BLUE);   // Status bar
    init_pair(6, COLOR_RED, COLOR_BLACK);    // Changed bytes
  }
  raw(); // Ctrl+C is the key 3: it asks about unsaved changes, it doesn't kill the program
  noecho();
  keypad(stdscr, TRUE);
  curs_set(0);
  werase(stdscr);
  wrefresh(stdscr);

  Ui ui;
  layoutWindows(ui, doc);
  bool running = true;
  int errors = 0; // keys that did not come: a signal, or the terminal is gone and it would go on for ever
  while (running && !gTerminate) {
    if (ui.tooSmall) {
      werase(stdscr);
      mvaddstr(0, 0, "Terminal too small");
      wrefresh(stdscr);
      const int key = getch();
      if (key == ERR) { if (++errors > 100) { break; } continue; }
      errors = 0;
      if (key == KEY_RESIZE) { layoutWindows(ui, doc); }
      else if (key == 'q' || key == 3) { break; }
      continue;
    }
    ensureVisible(ui, doc);
    drawHexView(ui.hex, doc, ui);
    drawStatus(ui.status, doc, filename, ui);
    const int ch = wgetch(ui.status);
    if (ch == ERR) { if (++errors > 100) { break; } continue; } // a signal came in: the loop condition looks at it
    errors = 0;
    ui.message.clear();
    if (ch == KEY_RESIZE) { layoutWindows(ui, doc); continue; }
    running = handleKey(ui, doc, filename, ch);
  }

  if (ui.hex) { delwin(ui.hex); }
  if (ui.status) { delwin(ui.status); }
  endwin();
  return EXIT_SUCCESS;
}