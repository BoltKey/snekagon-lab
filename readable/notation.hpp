// Text forms of moves and positions.
//
//   move:      "<piece>:<cell>,<cell>,..."  e.g. "4B:16,27,28,39" (cells ascending), or "pass"
//   position:  "<red cells>_<blue cells>_<red pieces>_<blue pieces>_<side>_<ply>_<passes>"
//              with cells as 32 hex digits and piece sets as decimal bit masks
//              (used in the training-data CSV files)
#pragma once
#include <cstdio>
#include <string>
#include <vector>
#include "rules.hpp"

inline std::string moveToString(Move m) {
    if (m.isPass()) return "pass";
    const Placement& p = m.placement();
    std::vector<int> cells(p.cellList, p.cellList + p.cellCount);
    std::sort(cells.begin(), cells.end());
    std::string text = std::string(PIECE_NAME[p.piece]) + ":";
    for (size_t j = 0; j < cells.size(); j++) {
        if (j) text += ',';
        text += std::to_string(cells[j]);
    }
    return text;
}

// Parses a move; false if the text does not name an existing placement (legality is not checked).
inline bool parseMove(const std::string& text, Move& out) {
    if (text == "pass") { out = Move::pass(); return true; }
    size_t colon = text.find(':');
    if (colon == std::string::npos) return false;
    std::string pieceName = text.substr(0, colon);
    int piece = -1;
    for (int p = 0; p < NUM_PIECES; p++)
        if (pieceName == PIECE_NAME[p]) piece = p;
    if (piece < 0) return false;
    Bitboard cells = 0;
    size_t start = colon + 1;
    while (start < text.size()) {
        size_t end = text.find(',', start);
        if (end == std::string::npos) end = text.size();
        int cell = std::atoi(text.substr(start, end - start).c_str());
        if (cell < 0 || cell >= GRID_SIZE) return false;
        cells |= cellBit(cell);
        start = end + 1;
    }
    for (const Placement& p : placementsOf(piece))
        if (p.cells == cells) { out = Move::place(p); return true; }
    return false;
}

// ---- positions -------------------------------------------------------------------------------

inline std::string bitboardToHex(Bitboard b) {
    char text[40];
    snprintf(text, sizeof text, "%016llx%016llx", (unsigned long long)(uint64_t)(b >> 64), (unsigned long long)(uint64_t)b);
    return text;
}

inline Bitboard hexToBitboard(const std::string& hex) {
    Bitboard b = 0;
    for (char c : hex) b = b * 16 + (Bitboard)(c >= '0' && c <= '9' ? c - '0' : c - 'a' + 10);
    return b;
}

inline std::string positionToString(const Position& pos) {
    return bitboardToHex(pos.cells[RED]) + "_" + bitboardToHex(pos.cells[BLUE]) + "_" +
           std::to_string(pos.piecesLeft[RED]) + "_" + std::to_string(pos.piecesLeft[BLUE]) + "_" +
           std::to_string(pos.sideToMove) + "_" + std::to_string(pos.ply) + "_" + std::to_string(pos.passesInRow);
}

inline std::vector<std::string> splitString(const std::string& text, char separator) {
    std::vector<std::string> parts;
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find(separator, start);
        if (end == std::string::npos) end = text.size();
        parts.push_back(text.substr(start, end - start));
        start = end + 1;
    }
    return parts;
}

inline bool parsePosition(const std::string& text, Position& pos) {
    std::vector<std::string> field = splitString(text, '_');
    if (field.size() != 7) return false;
    pos = Position{};
    pos.cells[RED] = hexToBitboard(field[0]);
    pos.cells[BLUE] = hexToBitboard(field[1]);
    pos.piecesLeft[RED] = (PieceSet)std::stoi(field[2]);
    pos.piecesLeft[BLUE] = (PieceSet)std::stoi(field[3]);
    pos.sideToMove = (uint8_t)std::stoi(field[4]);
    pos.ply = (uint8_t)std::stoi(field[5]);
    pos.passesInRow = (uint8_t)std::stoi(field[6]);
    pos.recomputeDerivedState();
    return true;
}

// Parses `text` and plays it if it is legal in `pos`; false (and `pos` unchanged) otherwise.
inline bool playMoveText(Position& pos, const std::string& text) {
    Move m;
    if (!parseMove(text, m) || !isLegalMove(pos, m)) return false;
    pos.play(m);
    return true;
}
