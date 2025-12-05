/*
 * Copyright 12/05/2025 https://github.com/su8/0verhex
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA 02110-1301, USA.
 */
#include <iostream>
#include <fstream>
#include <vector>
#include <iomanip>
#include <sstream>
#include <cctype>
#include <string>
#include <cstdlib>
#include <algorithm>
#include <stack>
#include <limits>
#include <ncurses.h>

// Structure to store an edit operation
struct Edit {
  size_t offset;
  unsigned char oldValue;
  unsigned char newValue;
};

// File I/O
bool readFile(const std::string &filename, std::vector<unsigned char> &buffer);
bool writeFile(const std::string &filename, const std::vector<unsigned char> &buffer);

// Draw functions
void drawHexView(WINDOW *win, const std::vector<unsigned char> &buffer, size_t start, size_t cursor, size_t bytesPerLine);
void drawStatus(WINDOW *status, const std::string &filename, size_t cursor, size_t filesize, bool modified);
std::string prompt(WINDOW *statusWin, const std::string &msg);

// Searching functions
size_t searchText(const std::vector<unsigned char> &buffer, const std::string &text, size_t start);
size_t searchHex(const std::vector<unsigned char> &buffer, const std::vector<unsigned char> &pattern, size_t start);

// Edit a byte and push to undo stack
void editByte(std::vector<unsigned char> &buffer, size_t offset, unsigned int newValue);
// Undo last edit
void undo(std::vector<unsigned char> &buffer);
// Redo last undone edit
void redo(std::vector<unsigned char> &buffer);

// Undo/Redo actions
std::stack<Edit> undoStack;
std::stack<Edit> redoStack;

// The hex and status line windows
WINDOW *hexWin;
WINDOW *statusWin;

int main(void) {
  std::string filename;
  std::cout << "Enter file name: ";
  std::getline(std::cin, filename);
  std::vector<unsigned char> buffer;
  if (!readFile(filename, buffer)) {
    std::cerr << "Error: Cannot open file." << std::endl;
    return EXIT_FAILURE;
  }
  std::vector<bool> modifiedFlags(buffer.size(), false);
  initscr();
  // Enable colors
  if (has_colors()) {
    start_color();
    init_pair(1, COLOR_CYAN,   COLOR_BLACK); // Offset
    init_pair(2, COLOR_YELLOW, COLOR_BLACK); // Hex
    init_pair(3, COLOR_GREEN,  COLOR_BLACK); // ASCII printable
    init_pair(4, COLOR_WHITE,  COLOR_BLACK); // ASCII non-printable
    init_pair(5, COLOR_WHITE,  COLOR_BLUE);  // Status bar
  }
  noecho();
  cbreak();
  keypad(stdscr, TRUE);
  curs_set(0);
  int rows, cols;
  getmaxyx(stdscr, rows, cols);
  size_t cursor = 0;
  size_t start = 0;
  const int bytesPerLine = 16;
  bool running = true;
  bool modified = false;
  hexWin = newwin(rows - 1, cols, 0, 0);
  statusWin = newwin(1, cols, rows - 1, 0);
  werase(stdscr);
  wrefresh(stdscr);
  while (running) {
    drawHexView(hexWin, buffer, start, cursor, bytesPerLine);
    drawStatus(statusWin, filename, cursor, buffer.size(), modified);
    int ch = getch();
    switch (ch) {
      case KEY_UP: {
        if (cursor >= bytesPerLine) cursor -= bytesPerLine;
        if (cursor < start) start -= bytesPerLine;
      }
      break;
      case KEY_DOWN: {
        if (cursor + bytesPerLine < buffer.size()) cursor += bytesPerLine;
        if (cursor >= start + (rows - 2) * bytesPerLine) start += bytesPerLine;
      }
      break;
      case KEY_LEFT: {
        if (cursor > 0) cursor--;
        if (cursor < start) start -= bytesPerLine;
      }
      break;
      case KEY_RIGHT: {
        if (cursor + 1 < buffer.size()) cursor++;
        if (cursor >= start + (rows - 2) * bytesPerLine) start += bytesPerLine;
      }
      break;
      case KEY_NPAGE: { // Page Down
        if (cursor + (rows - 2) * bytesPerLine < buffer.size())
          cursor += (rows - 2) * bytesPerLine;
        start = std::min(start + (rows - 2) * bytesPerLine, buffer.size() - 1);
      }
      break;
      case KEY_PPAGE: { // Page Up
        if (cursor >= (rows - 2) * bytesPerLine)
          cursor -= (rows - 2) * bytesPerLine;
        else
          cursor = 0;
        if (start >= (rows - 2) * bytesPerLine)
          start -= (rows - 2) * bytesPerLine;
        else
          start = 0;
      }
      break;
      case 'u': { // undo change
        undo(buffer, undoStack, redoStack);
      }
      break;
      case 'r': { // redo change
        redo(buffer, undoStack, redoStack);
      }
      break;
      case 'e': { // Edit byte
        std::string valStr = prompt(statusWin, "Enter new hex value (00-FF): ");
        unsigned int val;
        std::stringstream ss;
        ss << std::hex << valStr;
        if (ss >> val && val <= 0xFF) {
          buffer[cursor] = static_cast<unsigned char>(val);
          modifiedFlags[cursor] = true;
          modified = true;
          editByte(buffer, val, val);
        }
      }
      break;
      case 'i': { // Insert bytes
        std::string hexStr = prompt(statusWin, "Enter hex bytes to insert (e.g., 41 42 43): ");
        std::istringstream iss(hexStr);
        unsigned int byte;
        std::vector<unsigned char> data;
        while (iss >> std::hex >> byte)
          data.push_back(static_cast<unsigned char>(byte));
        buffer.insert(buffer.begin() + cursor, data.begin(), data.end());
        modifiedFlags.insert(modifiedFlags.begin() + cursor, data.size(), true);
        modified = true;
      }
      break;
      case 'd': { // Delete byte
        if (!buffer.empty()) {
          buffer.erase(buffer.begin() + cursor);
          modifiedFlags.erase(modifiedFlags.begin() + cursor);
          if (cursor >= buffer.size() && cursor > 0) cursor--;
          modified = true;
        }
      }
      break;
      case '/': { // Search ASCII
        std::string text = prompt(statusWin, "Enter ASCII text to search: ");
        size_t pos = searchText(buffer, text, cursor + 1);
        if (pos != SIZE_MAX) {
          cursor = pos;
          if (cursor < start || cursor >= start + (rows - 2) * bytesPerLine)
            start = (cursor / bytesPerLine) * bytesPerLine;
        } else {
          prompt(statusWin, "Not found. Press Enter to continue.");
        }
      }
      break;
      case 'h': { // Search hex sequence
        std::string hexStr = prompt(statusWin, "Enter hex sequence (e.g., 48 65 6C): ");
        std::istringstream iss(hexStr);
        unsigned int byte;
        std::vector<unsigned char> pattern;
        while (iss >> std::hex >> byte)
          pattern.push_back(static_cast<unsigned char>(byte));
        size_t pos = searchHex(buffer, pattern, cursor + 1);
        if (pos != SIZE_MAX) {
          cursor = pos;
          if (cursor < start || cursor >= start + (rows - 2) * bytesPerLine)
            start = (cursor / bytesPerLine) * bytesPerLine;
        } else {
          prompt(statusWin, "Not found. Press Enter to continue.");
        }
      }
      break;
      case 's': // Save
        if (writeFile(filename, buffer)) {
          modified = false;
          std::fill(modifiedFlags.begin(), modifiedFlags.end(), false);
          prompt(statusWin, "File saved. Press Enter to continue.");
        } else {
          prompt(statusWin, "Error saving file! Press Enter to continue.");
        }
        break;
      case 'q': // Quit
        if (modified) {
          std::string confirm = prompt(statusWin, "Unsaved changes! Type 'yes' to quit: ");
          if (confirm == "yes") running = false;
        } else {
          running = false;
        }
        break;
      default:
        break;
    }
  }
  delwin(hexWin);
  delwin(statusWin);
  endwin();
  return EXIT_SUCCESS;
}

// Edit a byte and push to undo stack
void editByte(std::vector<unsigned char> &buffer, size_t offset, unsigned int newValue) {
  if (offset >= buffer.size()) {
    prompt(statusWin, "Error: Offset out of range. Press enter to continue.");
    return;
  }
  if (newValue > 0xFF) {
    prompt(statusWin, "Error: Value must be between 0x00 and 0xFF. Press enter to continue.");
    return;
  }
  Edit e{offset, buffer[offset], static_cast<unsigned char>(newValue)};
  buffer[offset] = e.newValue;
  undoStack.push(e);
  while (!redoStack.empty()) redoStack.pop(); // Clear redo stack after new edit
}

// Undo last edit
void undo(std::vector<unsigned char> &buffer) {
  if (undoStack.empty()) {
    prompt(statusWin, "Nothing to undo. Press enter to continue.");
    return;
  }
  Edit e = undoStack.top();
  undoStack.pop();
  buffer[e.offset] = e.oldValue;
  redoStack.push(e);
}

// Redo last undone edit
void redo(std::vector<unsigned char> &buffer) {
   if (redoStack.empty()) {
    prompt(statusWin, "Nothing to redo. Press enter to continue.");
    return;
  }
  Edit e = redoStack.top();
  redoStack.pop();
  buffer[e.offset] = e.newValue;
  undoStack.push(e);
}

// Load file into buffer
bool readFile(const std::string &filename, std::vector<unsigned char> &buffer) {
  std::ifstream file(filename, std::ios::binary);
  if (!file) return false;
  buffer.assign(std::istreambuf_iterator<char>(file), {});
  return true;
}

// Save buffer to file
bool writeFile(const std::string &filename, const std::vector<unsigned char> &buffer) {
  std::ofstream file(filename, std::ios::binary);
  if (!file) return false;
  file.write(reinterpret_cast<const char*>(buffer.data()), buffer.size());
  return true;
}

// Draw hex + ASCII view with colors
void drawHexView(WINDOW *win, const std::vector<unsigned char> &buffer, size_t start, size_t cursor, size_t bytesPerLine) {
  werase(win);
  int rows, cols;
  getmaxyx(win, rows, cols);
  for (int row = 0; row < rows - 1; ++row) {
    size_t offset = start + row * bytesPerLine;
    if (offset >= buffer.size()) break;
    // Offset column (cyan)
    wattron(win, COLOR_PAIR(1));
    mvwprintw(win, row, 0, "%08zx", offset);
    wattroff(win, COLOR_PAIR(1));
    wprintw(win, "  ");
    // Hex bytes (yellow, highlight cursor)
    for (size_t i = 0; i < bytesPerLine; ++i) {
      if (offset + i < buffer.size()) {
        if (offset + i == cursor) {
          wattron(win, A_REVERSE | COLOR_PAIR(2));
          mvwprintw(win, row, 10 + i * 3, "%02X", buffer[offset + i]);
          wattroff(win, A_REVERSE | COLOR_PAIR(2));
        } else {
          wattron(win, COLOR_PAIR(2));
          mvwprintw(win, row, 10 + i * 3, "%02X", buffer[offset + i]);
          wattroff(win, COLOR_PAIR(2));
        }
      } else {
          mvwprintw(win, row, 10 + i * 3, "  ");
      }
    }
    // ASCII representation (green for printable, dim gray for non-printable)
    for (size_t i = 0; i < bytesPerLine; ++i) {
      if (offset + i < buffer.size()) {
        unsigned char c = buffer[offset + i];
        if (std::isprint(c)) {
          wattron(win, COLOR_PAIR(3));
          mvwaddch(win, row, 10 + bytesPerLine * 3 + 2 + i, c);
          wattroff(win, COLOR_PAIR(3));
        } else {
          wattron(win, COLOR_PAIR(4));
          mvwaddch(win, row, 10 + bytesPerLine * 3 + 2 + i, '.');
          wattroff(win, COLOR_PAIR(4));
        }
      }
    }
  }
  wrefresh(win);
}

// Draw status bar (white on blue)
void drawStatus(WINDOW *status, const std::string &filename, size_t cursor, size_t filesize, bool modified) {
  werase(status);
  wattron(status, COLOR_PAIR(5));
  mvwprintw(status, 0, 0, "File: %s | Size: %zu bytes | Cursor: 0x%zx%s | q=Quit s=Save e=Edit i=Insert d=Delete / SearchASCII h=SearchHex u=Undo r=Redo", filename.c_str(), filesize, cursor, modified ? " [MODIFIED]" : "");
  wattroff(status, COLOR_PAIR(5));
  wrefresh(status);
}

// Prompt user for input
std::string prompt(WINDOW *statusWin, const std::string &msg) {
  char buf[256] = {'\0'};
  werase(statusWin);
  mvwprintw(statusWin, 0, 0, "%s", msg.c_str());
  wclrtoeol(statusWin);
  wrefresh(statusWin);
  echo();
  curs_set(1);
  wgetnstr(statusWin, buf, 255);
  noecho();
  curs_set(0);
  return std::string(buf);
}

// Search ASCII text
size_t searchText(const std::vector<unsigned char> &buffer, const std::string &text, size_t start) {
  auto it = std::search(buffer.begin() + start, buffer.end(), text.begin(), text.end());
  if (it != buffer.end()) return std::distance(buffer.begin(), it);
  return SIZE_MAX;
}

// Search hex sequence
size_t searchHex(const std::vector<unsigned char> &buffer, const std::vector<unsigned char> &pattern, size_t start) {
  auto it = std::search(buffer.begin() + start, buffer.end(), pattern.begin(), pattern.end());
  if (it != buffer.end()) return std::distance(buffer.begin(), it);
  return SIZE_MAX;
}