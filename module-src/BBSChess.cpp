// BBSChess.cpp — Chess-by-mail module for Meshtastic BBS (ESP32/Heltec V3)
// Implements: board init, move generation, alpha-beta minimax AI,
//             board rendering, LittleFS persistence, Elo ratings.

#include "BBSChess.h"
#include "BBSPlatform.h"
#include "FSCommon.h"
#include "RTC.h"
#include <Arduino.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cctype>

// ---------------------------------------------------------------------------
// Piece emoji table (indexed by piece+6, piece in -6..+6)
// ---------------------------------------------------------------------------
// [0]=black king ... [6]=nullptr(empty) ... [12]=white king
const char *CHESS_PIECE_EMOJI[13] = {
    "\xE2\x99\x9A",  // [0]  ♚ black king   U+265A
    "\xE2\x99\x9B",  // [1]  ♛ black queen  U+265B
    "\xE2\x99\x9C",  // [2]  ♜ black rook   U+265C
    "\xE2\x99\x9D",  // [3]  ♝ black bishop U+265D
    "\xE2\x99\x9E",  // [4]  ♞ black knight U+265E
    "\xE2\x99\x9F",  // [5]  ♟ black pawn   U+265F
    nullptr,          // [6]  empty square
    "\xE2\x99\x99",  // [7]  ♙ white pawn   U+2659
    "\xE2\x99\x98",  // [8]  ♘ white knight U+2658
    "\xE2\x99\x97",  // [9]  ♗ white bishop U+2657
    "\xE2\x99\x96",  // [10] ♖ white rook   U+2656
    "\xE2\x99\x95",  // [11] ♕ white queen  U+2655
    "\xE2\x99\x94",  // [12] ♔ white king   U+2654
};
const char *CHESS_SQ_WHITE = "\xE2\x97\xBB";  // ◻ U+25FB medium white square
const char *CHESS_SQ_BLACK = "\xE2\x97\xBC";  // ◼ U+25FC medium black square

// ---------------------------------------------------------------------------
// Internal move struct
// ---------------------------------------------------------------------------
struct ChessMove {
    int8_t fr, ff, tr, tf, promo; // promo: piece type 2-5, 0=none
};
static const int MAX_MOVES = 220;

// ---------------------------------------------------------------------------
// Piece-square tables (white's perspective; black uses mirrored rank)
// ---------------------------------------------------------------------------
static const int8_t PST_PAWN[8][8] = {
    { 0,  0,  0,  0,  0,  0,  0,  0},
    {-5, 10, 10,-20,-20, 10, 10, -5},
    {-5, -5,-10,  0,  0,-10, -5, -5},
    { 0,  0,  0, 20, 20,  0,  0,  0},
    { 5,  5, 10, 25, 25, 10,  5,  5},
    {10, 10, 20, 30, 30, 20, 10, 10},
    {50, 50, 50, 50, 50, 50, 50, 50},
    { 0,  0,  0,  0,  0,  0,  0,  0},
};
static const int8_t PST_KNIGHT[8][8] = {
    {-50,-40,-30,-30,-30,-30,-40,-50},
    {-40,-20,  0,  5,  5,  0,-20,-40},
    {-30,  5, 10, 15, 15, 10,  5,-30},
    {-30,  0, 15, 20, 20, 15,  0,-30},
    {-30,  5, 15, 20, 20, 15,  5,-30},
    {-30,  0, 10, 15, 15, 10,  0,-30},
    {-40,-20,  0,  0,  0,  0,-20,-40},
    {-50,-40,-30,-30,-30,-30,-40,-50},
};
static const int8_t PST_BISHOP[8][8] = {
    {-20,-10,-10,-10,-10,-10,-10,-20},
    {-10,  5,  0,  0,  0,  0,  5,-10},
    {-10, 10, 10, 10, 10, 10, 10,-10},
    {-10,  0, 10, 10, 10, 10,  0,-10},
    {-10,  5,  5, 10, 10,  5,  5,-10},
    {-10,  0,  5, 10, 10,  5,  0,-10},
    {-10,  0,  0,  0,  0,  0,  0,-10},
    {-20,-10,-10,-10,-10,-10,-10,-20},
};
static const int8_t PST_ROOK[8][8] = {
    { 0,  0,  0,  5,  5,  0,  0,  0},
    {-5,  0,  0,  0,  0,  0,  0, -5},
    {-5,  0,  0,  0,  0,  0,  0, -5},
    {-5,  0,  0,  0,  0,  0,  0, -5},
    {-5,  0,  0,  0,  0,  0,  0, -5},
    {-5,  0,  0,  0,  0,  0,  0, -5},
    { 5, 10, 10, 10, 10, 10, 10,  5},
    { 0,  0,  0,  0,  0,  0,  0,  0},
};
static const int8_t PST_QUEEN[8][8] = {
    {-20,-10,-10, -5, -5,-10,-10,-20},
    {-10,  0,  5,  0,  0,  0,  0,-10},
    {-10,  5,  5,  5,  5,  5,  0,-10},
    {  0,  0,  5,  5,  5,  5,  0, -5},
    { -5,  0,  5,  5,  5,  5,  0, -5},
    {-10,  0,  5,  5,  5,  5,  0,-10},
    {-10,  0,  0,  0,  0,  0,  0,-10},
    {-20,-10,-10, -5, -5,-10,-10,-20},
};
static const int8_t PST_KING[8][8] = {
    { 20, 30, 10,  0,  0, 10, 30, 20},
    { 20, 20,  0,  0,  0,  0, 20, 20},
    {-10,-20,-20,-20,-20,-20,-20,-10},
    {-20,-30,-30,-40,-40,-30,-30,-20},
    {-30,-40,-40,-50,-50,-40,-40,-30},
    {-30,-40,-40,-50,-50,-40,-40,-30},
    {-30,-40,-40,-50,-50,-40,-40,-30},
    {-30,-40,-40,-50,-50,-40,-40,-30},
};

// Material values
static const int PIECE_VALUE[7] = {0, 100, 320, 330, 500, 900, 20000};

// ---------------------------------------------------------------------------
// chessEnsureDir
// ---------------------------------------------------------------------------
void chessEnsureDir() {
    if (!FSCom.exists(BBS_CHESS_DIR)) {
        FSCom.mkdir(BBS_CHESS_DIR);
    }
}

// ---------------------------------------------------------------------------
// chessBoardInit — set up the standard starting position
// ---------------------------------------------------------------------------
void chessBoardInit(ChessBoard b) {
    memset(b, 0, sizeof(ChessBoard));
    // White back rank: rank 0
    b[0][0] =  4; b[0][1] =  2; b[0][2] =  3; b[0][3] =  5;
    b[0][4] =  6; b[0][5] =  3; b[0][6] =  2; b[0][7] =  4;
    // White pawns: rank 1
    for (int f = 0; f < 8; f++) b[1][f] = 1;
    // Black pawns: rank 6
    for (int f = 0; f < 8; f++) b[6][f] = -1;
    // Black back rank: rank 7
    b[7][0] = -4; b[7][1] = -2; b[7][2] = -3; b[7][3] = -5;
    b[7][4] = -6; b[7][5] = -3; b[7][6] = -2; b[7][7] = -4;
}

// ---------------------------------------------------------------------------
// chessParseMove — parse "e2e4" or "e7e8q" into rank/file indices
// Returns false on invalid input.
// Ranks: '1'-'8' -> 0-7   Files: 'a'-'h' -> 0-7
// ---------------------------------------------------------------------------
bool chessParseMove(const char *move, int *fr, int *ff, int *tr, int *tf, int8_t *promo) {
    if (!move) return false;
    size_t len = strlen(move);
    if (len < 4) return false;
    char fc = (char)tolower((unsigned char)move[0]);
    char rc = move[1];
    char tc = (char)tolower((unsigned char)move[2]);
    char trc = move[3];
    if (fc < 'a' || fc > 'h') return false;
    if (tc < 'a' || tc > 'h') return false;
    if (rc < '1' || rc > '8') return false;
    if (trc < '1' || trc > '8') return false;
    *ff = fc - 'a';
    *fr = rc - '1';
    *tf = tc - 'a';
    *tr = trc - '1';
    *promo = 0;
    if (len >= 5) {
        char pc = (char)tolower((unsigned char)move[4]);
        switch (pc) {
            case 'q': *promo = 5; break;
            case 'r': *promo = 4; break;
            case 'b': *promo = 3; break;
            case 'n': *promo = 2; break;
            default:  *promo = 0; break;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// chessMoveToStr — format a move as "e2e4" or "e7e8q"
// ---------------------------------------------------------------------------
void chessMoveToStr(int fr, int ff, int tr, int tf, int8_t promo, char *out) {
    out[0] = (char)('a' + ff);
    out[1] = (char)('1' + fr);
    out[2] = (char)('a' + tf);
    out[3] = (char)('1' + tr);
    if (promo > 0) {
        const char promoChars[] = "  nbrq";
        out[4] = (promo >= 2 && promo <= 5) ? promoChars[promo] : 'q';
        out[5] = '\0';
    } else {
        out[4] = '\0';
    }
}

// ---------------------------------------------------------------------------
// chessIsInCheck — check if the king of the given color is under attack
// ---------------------------------------------------------------------------
bool chessIsInCheck(const ChessBoard b, bool whiteKing) {
    // Find the king
    int8_t king = whiteKing ? 6 : -6;
    int kr = -1, kf = -1;
    for (int r = 0; r < 8 && kr < 0; r++) {
        for (int f = 0; f < 8 && kr < 0; f++) {
            if (b[r][f] == king) { kr = r; kf = f; }
        }
    }
    if (kr < 0) return false; // no king found (shouldn't happen)

    // Knight attacks
    static const int knightDr[8] = {-2,-2,-1,-1, 1, 1, 2, 2};
    static const int knightDf[8] = {-1, 1,-2, 2,-2, 2,-1, 1};
    int8_t enemyKnight = whiteKing ? -2 : 2;
    for (int i = 0; i < 8; i++) {
        int nr = kr + knightDr[i];
        int nf = kf + knightDf[i];
        if (nr >= 0 && nr < 8 && nf >= 0 && nf < 8) {
            if (b[nr][nf] == enemyKnight) return true;
        }
    }

    // Pawn attacks
    // White king is attacked by black pawns (which sit at rank-1 relative to white)
    // Black pawns attack downward (toward rank 0), so they threaten squares at rank+1
    int8_t enemyPawn = whiteKing ? -1 : 1;
    int pawnDir = whiteKing ? 1 : -1; // direction the enemy pawn moves (= direction it attacks from)
    {
        int pr = kr + pawnDir;
        if (pr >= 0 && pr < 8) {
            if (kf - 1 >= 0 && b[pr][kf-1] == enemyPawn) return true;
            if (kf + 1 <  8 && b[pr][kf+1] == enemyPawn) return true;
        }
    }

    // King adjacency
    int8_t enemyKing = whiteKing ? -6 : 6;
    for (int dr = -1; dr <= 1; dr++) {
        for (int df = -1; df <= 1; df++) {
            if (dr == 0 && df == 0) continue;
            int nr = kr + dr;
            int nf = kf + df;
            if (nr >= 0 && nr < 8 && nf >= 0 && nf < 8) {
                if (b[nr][nf] == enemyKing) return true;
            }
        }
    }

    // Sliding pieces: 8 directions
    static const int slideDir[8][2] = {
        {1,0},{-1,0},{0,1},{0,-1},{1,1},{1,-1},{-1,1},{-1,-1}
    };
    for (int d = 0; d < 8; d++) {
        int dr = slideDir[d][0];
        int df = slideDir[d][1];
        bool diagonal = (dr != 0 && df != 0);
        int nr = kr + dr;
        int nf = kf + df;
        while (nr >= 0 && nr < 8 && nf >= 0 && nf < 8) {
            int8_t piece = b[nr][nf];
            if (piece != 0) {
                // Is this an enemy slider?
                bool isEnemy = whiteKing ? (piece < 0) : (piece > 0);
                if (isEnemy) {
                    int8_t abs_piece = piece < 0 ? -piece : piece;
                    // Queen (5) threatens in all 8 directions
                    if (abs_piece == 5) { return true; }
                    // Rook (4) threatens in orthogonal directions
                    if (abs_piece == 4 && !diagonal) { return true; }
                    // Bishop (3) threatens in diagonal directions
                    if (abs_piece == 3 && diagonal) { return true; }
                }
                break; // any piece (own or enemy) blocks further rays
            }
            nr += dr;
            nf += df;
        }
    }

    return false;
}

// ---------------------------------------------------------------------------
// boardApplyRaw — apply a move to produce a new board state
// Updates castling rights and en-passant file.
// ---------------------------------------------------------------------------
static void boardApplyRaw(ChessBoard dst, const ChessBoard src,
                           int fr, int ff, int tr, int tf, int8_t promo,
                           uint8_t &castling, int8_t &epFile) {
    memcpy(dst, src, sizeof(ChessBoard));

    int8_t piece = dst[fr][ff];
    int8_t captured = dst[tr][tf];

    dst[tr][tf] = piece;
    dst[fr][ff] = 0;

    // En-passant capture: pawn moved diagonally to an empty square
    if ((piece == 1 || piece == -1) && ff != tf && captured == 0) {
        // Remove the captured pawn which is on the same rank as the moving pawn
        dst[fr][tf] = 0;
    }

    // Castling: king moved two files
    if (piece == 6 || piece == -6) {
        int fileDiff = tf - ff;
        if (fileDiff == 2) {
            // King-side: rook from file 7 to file 5
            dst[tr][5] = dst[tr][7];
            dst[tr][7] = 0;
        } else if (fileDiff == -2) {
            // Queen-side: rook from file 0 to file 3
            dst[tr][3] = dst[tr][0];
            dst[tr][0] = 0;
        }
    }

    // Promotion
    if (promo != 0) {
        if (piece == 1 && tr == 7) {
            dst[tr][tf] = (int8_t)promo;
        } else if (piece == -1 && tr == 0) {
            dst[tr][tf] = (int8_t)(-promo);
        }
    }

    // Update en-passant file
    if ((piece == 1 && fr == 1 && tr == 3) ||
        (piece == -1 && fr == 6 && tr == 4)) {
        epFile = (int8_t)ff;
    } else {
        epFile = -1;
    }

    // Update castling rights
    // White king moved
    if (piece == 6 && fr == 0 && ff == 4) {
        castling &= ~(uint8_t)0x03; // clear WK and WQ
    }
    // Black king moved
    if (piece == -6 && fr == 7 && ff == 4) {
        castling &= ~(uint8_t)0x0C; // clear BK and BQ
    }
    // Rook moved from corners
    if (fr == 0 && ff == 7) castling &= ~(uint8_t)0x01; // WK rook
    if (fr == 0 && ff == 0) castling &= ~(uint8_t)0x02; // WQ rook
    if (fr == 7 && ff == 7) castling &= ~(uint8_t)0x04; // BK rook
    if (fr == 7 && ff == 0) castling &= ~(uint8_t)0x08; // BQ rook
    // Rook captured on corners
    if (tr == 0 && tf == 7) castling &= ~(uint8_t)0x01;
    if (tr == 0 && tf == 0) castling &= ~(uint8_t)0x02;
    if (tr == 7 && tf == 7) castling &= ~(uint8_t)0x04;
    if (tr == 7 && tf == 0) castling &= ~(uint8_t)0x08;
}

// ---------------------------------------------------------------------------
// generatePseudoLegal — generate pseudo-legal moves for the given side
// Returns the count of moves generated into the moves[] array.
// ---------------------------------------------------------------------------
static int generatePseudoLegal(const ChessBoard b, uint8_t castling, int8_t epFile,
                                bool forWhite, ChessMove *moves) {
    int count = 0;

    auto addMove = [&](int fr, int ff, int tr, int tf, int8_t promo) {
        if (count < MAX_MOVES) {
            moves[count++] = {(int8_t)fr, (int8_t)ff, (int8_t)tr, (int8_t)tf, promo};
        }
    };

    for (int r = 0; r < 8; r++) {
        for (int f = 0; f < 8; f++) {
            int8_t piece = b[r][f];
            if (piece == 0) continue;
            bool isWhite = (piece > 0);
            if (isWhite != forWhite) continue;

            int8_t abs_piece = piece < 0 ? -piece : piece;

            switch (abs_piece) {
                case 1: { // Pawn
                    int dir = forWhite ? 1 : -1;
                    int startRank = forWhite ? 1 : 6;
                    int promoRank = forWhite ? 7 : 0;

                    // Forward one
                    int nr = r + dir;
                    if (nr >= 0 && nr < 8 && b[nr][f] == 0) {
                        if (nr == promoRank) {
                            addMove(r, f, nr, f, 5);
                            addMove(r, f, nr, f, 4);
                            addMove(r, f, nr, f, 3);
                            addMove(r, f, nr, f, 2);
                        } else {
                            addMove(r, f, nr, f, 0);
                            // Forward two from starting rank
                            if (r == startRank) {
                                int nr2 = r + 2 * dir;
                                if (nr2 >= 0 && nr2 < 8 && b[nr2][f] == 0) {
                                    addMove(r, f, nr2, f, 0);
                                }
                            }
                        }
                    }

                    // Diagonal captures
                    for (int df = -1; df <= 1; df += 2) {
                        int nf2 = f + df;
                        if (nf2 < 0 || nf2 >= 8) continue;
                        int nr2 = r + dir;
                        if (nr2 < 0 || nr2 >= 8) continue;
                        int8_t target = b[nr2][nf2];
                        bool isEnemy = forWhite ? (target < 0) : (target > 0);
                        if (isEnemy) {
                            if (nr2 == promoRank) {
                                addMove(r, f, nr2, nf2, 5);
                                addMove(r, f, nr2, nf2, 4);
                                addMove(r, f, nr2, nf2, 3);
                                addMove(r, f, nr2, nf2, 2);
                            } else {
                                addMove(r, f, nr2, nf2, 0);
                            }
                        }

                        // En passant
                        // White pawn captures en passant from rank 4; black from rank 3
                        int epSrcRank = forWhite ? 4 : 3;
                        int epDstRank = forWhite ? 5 : 2;
                        if (r == epSrcRank && epFile == nf2 && b[nr2][nf2] == 0) {
                            addMove(r, f, epDstRank, nf2, 0);
                        }
                    }
                    break;
                }

                case 2: { // Knight
                    static const int knightDr[8] = {-2,-2,-1,-1, 1, 1, 2, 2};
                    static const int knightDf[8] = {-1, 1,-2, 2,-2, 2,-1, 1};
                    for (int i = 0; i < 8; i++) {
                        int nr = r + knightDr[i];
                        int nf = f + knightDf[i];
                        if (nr < 0 || nr >= 8 || nf < 0 || nf >= 8) continue;
                        int8_t target = b[nr][nf];
                        bool ownPiece = forWhite ? (target > 0) : (target < 0);
                        if (!ownPiece) addMove(r, f, nr, nf, 0);
                    }
                    break;
                }

                case 3: // Bishop
                case 4: // Rook
                case 5: { // Queen
                    static const int bishopDirs[4][2] = {{1,1},{1,-1},{-1,1},{-1,-1}};
                    static const int rookDirs[4][2]   = {{1,0},{-1,0},{0,1},{0,-1}};
                    bool doBishop = (abs_piece == 3 || abs_piece == 5);
                    bool doRook   = (abs_piece == 4 || abs_piece == 5);
                    if (doBishop) {
                        for (int d = 0; d < 4; d++) {
                            int nr = r + bishopDirs[d][0];
                            int nf = f + bishopDirs[d][1];
                            while (nr >= 0 && nr < 8 && nf >= 0 && nf < 8) {
                                int8_t target = b[nr][nf];
                                bool ownPiece = forWhite ? (target > 0) : (target < 0);
                                if (ownPiece) break;
                                addMove(r, f, nr, nf, 0);
                                if (target != 0) break; // capture, stop ray
                                nr += bishopDirs[d][0];
                                nf += bishopDirs[d][1];
                            }
                        }
                    }
                    if (doRook) {
                        for (int d = 0; d < 4; d++) {
                            int nr = r + rookDirs[d][0];
                            int nf = f + rookDirs[d][1];
                            while (nr >= 0 && nr < 8 && nf >= 0 && nf < 8) {
                                int8_t target = b[nr][nf];
                                bool ownPiece = forWhite ? (target > 0) : (target < 0);
                                if (ownPiece) break;
                                addMove(r, f, nr, nf, 0);
                                if (target != 0) break;
                                nr += rookDirs[d][0];
                                nf += rookDirs[d][1];
                            }
                        }
                    }
                    break;
                }

                case 6: { // King
                    // Normal king moves
                    for (int dr = -1; dr <= 1; dr++) {
                        for (int df = -1; df <= 1; df++) {
                            if (dr == 0 && df == 0) continue;
                            int nr = r + dr;
                            int nf = f + df;
                            if (nr < 0 || nr >= 8 || nf < 0 || nf >= 8) continue;
                            int8_t target = b[nr][nf];
                            bool ownPiece = forWhite ? (target > 0) : (target < 0);
                            if (!ownPiece) addMove(r, f, nr, nf, 0);
                        }
                    }

                    // Castling
                    if (forWhite && r == 0 && f == 4) {
                        // White king-side: bits 0x01, squares f=5 and f=6 must be empty
                        if ((castling & 0x01) &&
                            b[0][5] == 0 && b[0][6] == 0) {
                            addMove(0, 4, 0, 6, 0);
                        }
                        // White queen-side: bits 0x02, squares f=1, f=2, f=3 must be empty
                        if ((castling & 0x02) &&
                            b[0][1] == 0 && b[0][2] == 0 && b[0][3] == 0) {
                            addMove(0, 4, 0, 2, 0);
                        }
                    }
                    if (!forWhite && r == 7 && f == 4) {
                        // Black king-side: bits 0x04
                        if ((castling & 0x04) &&
                            b[7][5] == 0 && b[7][6] == 0) {
                            addMove(7, 4, 7, 6, 0);
                        }
                        // Black queen-side: bits 0x08
                        if ((castling & 0x08) &&
                            b[7][1] == 0 && b[7][2] == 0 && b[7][3] == 0) {
                            addMove(7, 4, 7, 2, 0);
                        }
                    }
                    break;
                }
            }
        }
    }

    return count;
}

// ---------------------------------------------------------------------------
// generateLegal — filter pseudo-legal moves by checking for self-check.
// Also verifies castling doesn't pass through check.
// ---------------------------------------------------------------------------
static int generateLegal(const BBSChessGame &game, ChessMove *moves, int maxOut) {
    ChessMove pseudo[MAX_MOVES];
    bool forWhite = (game.toMove == 0);
    int pseudoCount = generatePseudoLegal(game.board, game.castling, game.enPassantFile,
                                          forWhite, pseudo);

    int count = 0;
    for (int i = 0; i < pseudoCount && count < maxOut; i++) {
        ChessMove &m = pseudo[i];
        ChessBoard tmp;
        uint8_t tmpCast = game.castling;
        int8_t  tmpEP   = game.enPassantFile;
        boardApplyRaw(tmp, game.board, m.fr, m.ff, m.tr, m.tf, m.promo, tmpCast, tmpEP);

        // After the move, the side that just moved must not be in check
        if (chessIsInCheck(tmp, forWhite)) continue;

        // Extra castling check: king must not pass through attacked square
        int8_t abs_piece = game.board[m.fr][m.ff];
        if (abs_piece < 0) abs_piece = -abs_piece;
        if (abs_piece == 6 && (m.tf - m.ff == 2 || m.tf - m.ff == -2)) {
            // King must not be in check at starting square
            if (chessIsInCheck(game.board, forWhite)) continue;

            // King must not pass through check on the intermediate square
            int passFile = (m.ff + m.tf) / 2;
            ChessBoard passBoard;
            uint8_t passCast = game.castling;
            int8_t  passEP   = game.enPassantFile;
            boardApplyRaw(passBoard, game.board, m.fr, m.ff, m.fr, passFile, 0, passCast, passEP);
            if (chessIsInCheck(passBoard, forWhite)) continue;
        }

        moves[count++] = m;
    }
    return count;
}

// ---------------------------------------------------------------------------
// chessHasLegalMoves
// ---------------------------------------------------------------------------
bool chessHasLegalMoves(const BBSChessGame &game) {
    ChessMove moves[MAX_MOVES];
    return generateLegal(game, moves, MAX_MOVES) > 0;
}

// ---------------------------------------------------------------------------
// chessCheckTermination — sets game.status if game has ended
// Returns the status value. 0=still active.
// ---------------------------------------------------------------------------
uint8_t chessCheckTermination(BBSChessGame &game) {
    if (game.status != 0) return game.status;

    bool forWhite = (game.toMove == 0);
    bool inCheck  = chessIsInCheck(game.board, forWhite);
    bool hasLegal = chessHasLegalMoves(game);

    if (!hasLegal) {
        if (inCheck) {
            // Checkmate: the side to move has lost
            game.status = forWhite ? 2 : 1; // 1=white won, 2=black won
        } else {
            game.status = 4; // stalemate
        }
    } else if (game.halfMoveClock >= 100) {
        game.status = 3; // 50-move rule draw
    }

    return game.status;
}

// ---------------------------------------------------------------------------
// chessApplyMove — apply a move string to the game state
// ---------------------------------------------------------------------------
bool chessApplyMove(BBSChessGame &game, const char *move) {
    int fr, ff, tr, tf;
    int8_t promo = 0;
    if (!chessParseMove(move, &fr, &ff, &tr, &tf, &promo)) return false;

    // Default promotion to queen if pawn reaches back rank and no promo specified
    if (promo == 0) {
        int8_t p = game.board[fr][ff];
        if ((p == 1 && tr == 7) || (p == -1 && tr == 0)) promo = 5;
    }

    // Generate legal moves and find a match
    ChessMove legal[MAX_MOVES];
    int lcount = generateLegal(game, legal, MAX_MOVES);

    bool found = false;
    ChessMove chosen;
    for (int i = 0; i < lcount; i++) {
        ChessMove &m = legal[i];
        if (m.fr == fr && m.ff == ff && m.tr == tr && m.tf == tf) {
            // Match promotion if specified; if promo==0 and move has promo, accept first
            if (promo == 0 || m.promo == 0 || m.promo == promo) {
                chosen = m;
                // If promo was given, force it
                if (promo != 0) chosen.promo = promo;
                found = true;
                break;
            }
        }
    }
    if (!found) return false;

    // Track capture and pawn move for half-move clock
    bool isCapture = (game.board[tr][tf] != 0);
    bool isPawn    = (game.board[fr][ff] == 1 || game.board[fr][ff] == -1);
    // En passant is also a pawn move+capture
    if (isPawn && ff != tf && !isCapture) isCapture = true;

    uint8_t newCast = game.castling;
    int8_t  newEP   = game.enPassantFile;
    boardApplyRaw(game.board, game.board, chosen.fr, chosen.ff, chosen.tr, chosen.tf,
                  chosen.promo, newCast, newEP);
    game.castling      = newCast;
    game.enPassantFile = newEP;

    // Update clocks
    if (isPawn || isCapture) {
        game.halfMoveClock = 0;
    } else {
        game.halfMoveClock++;
    }

    if (game.toMove == 1) game.fullMoveNumber++;
    game.toMove = (game.toMove == 0) ? 1 : 0;

    // Record last move string
    chessMoveToStr(chosen.fr, chosen.ff, chosen.tr, chosen.tf, chosen.promo, game.lastMoveStr);
    game.lastMove = getTime();

    return true;
}

// ---------------------------------------------------------------------------
// chessBuildBoard — 8x8 emoji grid, 200 bytes exactly
// ▫ U+25AB (light sq), ▪ U+25AA (dark sq) — both small squares, 3 bytes each
// ---------------------------------------------------------------------------
void chessBuildBoard(const BBSChessGame &game, char *buf, size_t bufLen, bool whiteAtBottom) {
    static const char *sqLight = "\xE2\x96\xAB";  // ▫ U+25AB white small square
    static const char *sqDark  = "\xE2\x96\xAA";  // ▪ U+25AA black small square

    size_t pos = 0;
    for (int row = 0; row < 8 && pos + 25 < bufLen; row++) {
        int rank = whiteAtBottom ? (7 - row) : row;
        for (int col = 0; col < 8; col++) {
            int file = whiteAtBottom ? col : (7 - col);
            int8_t piece = game.board[rank][file];
            if (piece == 0) {
                bool light = ((rank + file) % 2 == 1);
                memcpy(buf + pos, light ? sqLight : sqDark, 3);
            } else {
                const char *em = CHESS_PIECE_EMOJI[piece + 6];
                if (!em) em = sqLight;
                memcpy(buf + pos, em, 3);
            }
            pos += 3;
        }
        buf[pos++] = '\n';
    }
    if (pos < bufLen) buf[pos] = '\0';
    else if (bufLen > 0) buf[bufLen-1] = '\0';
}

// ---------------------------------------------------------------------------
// AI: evaluation function
// ---------------------------------------------------------------------------
static int chessEval(const ChessBoard b) {
    int score = 0;
    for (int r = 0; r < 8; r++) {
        for (int f = 0; f < 8; f++) {
            int8_t piece = b[r][f];
            if (piece == 0) continue;
            bool isWhite = (piece > 0);
            int8_t abs_p = piece < 0 ? -piece : piece;
            int mat = PIECE_VALUE[abs_p];
            // PST lookup: white uses rank as-is, black mirrors rank
            int pstRank = isWhite ? r : (7 - r);
            int pst = 0;
            switch (abs_p) {
                case 1: pst = PST_PAWN[pstRank][f];   break;
                case 2: pst = PST_KNIGHT[pstRank][f]; break;
                case 3: pst = PST_BISHOP[pstRank][f]; break;
                case 4: pst = PST_ROOK[pstRank][f];   break;
                case 5: pst = PST_QUEEN[pstRank][f];  break;
                case 6: pst = PST_KING[pstRank][f];   break;
                default: break;
            }
            if (isWhite) score += mat + pst;
            else         score -= mat + pst;
        }
    }
    return score;
}

// ---------------------------------------------------------------------------
// AI: search state and minimax
// ---------------------------------------------------------------------------
struct ChessSearchState {
    ChessBoard board;
    uint8_t castling;
    int8_t  epFile;
    uint8_t toMove;
};

static uint32_t s_searchDeadline;

static int chessMinimax(ChessSearchState &state, int depth, int alpha, int beta, bool maximizing) {
    // Time check
    if (millis() >= s_searchDeadline) {
        return chessEval(state.board);
    }

    if (depth == 0) {
        return chessEval(state.board);
    }

    bool forWhite = (state.toMove == 0);
    ChessMove moves[MAX_MOVES];

    // Build a temporary BBSChessGame for legal move generation
    BBSChessGame tmp;
    memset(&tmp, 0, sizeof(tmp));
    memcpy(tmp.board, state.board, sizeof(ChessBoard));
    tmp.castling      = state.castling;
    tmp.enPassantFile = state.epFile;
    tmp.toMove        = state.toMove;

    int count = generateLegal(tmp, moves, MAX_MOVES);

    if (count == 0) {
        // Terminal node
        if (chessIsInCheck(state.board, forWhite)) {
            // Checkmate: current side lost
            return maximizing ? (-20000 - depth) : (20000 + depth);
        }
        return 0; // stalemate
    }

    if (maximizing) {
        int best = -30000;
        for (int i = 0; i < count; i++) {
            ChessSearchState child;
            child.castling = state.castling;
            child.epFile   = state.epFile;
            child.toMove   = (state.toMove == 0) ? 1 : 0;
            boardApplyRaw(child.board, state.board,
                          moves[i].fr, moves[i].ff, moves[i].tr, moves[i].tf,
                          moves[i].promo, child.castling, child.epFile);
            int val = chessMinimax(child, depth - 1, alpha, beta, false);
            if (val > best) best = val;
            if (best > alpha) alpha = best;
            if (beta <= alpha) break;
            if (millis() >= s_searchDeadline) break;
        }
        return best;
    } else {
        int best = 30000;
        for (int i = 0; i < count; i++) {
            ChessSearchState child;
            child.castling = state.castling;
            child.epFile   = state.epFile;
            child.toMove   = (state.toMove == 0) ? 1 : 0;
            boardApplyRaw(child.board, state.board,
                          moves[i].fr, moves[i].ff, moves[i].tr, moves[i].tf,
                          moves[i].promo, child.castling, child.epFile);
            int val = chessMinimax(child, depth - 1, alpha, beta, true);
            if (val < best) best = val;
            if (best < beta) beta = best;
            if (beta <= alpha) break;
            if (millis() >= s_searchDeadline) break;
        }
        return best;
    }
}

// ---------------------------------------------------------------------------
// chessBuildFEN — encode game state as a FEN string (~50-90 chars)
// Piece encoding: 1=P,2=N,3=B,4=R,5=Q,6=K (positive=white, negative=black)
// FEN piece chars indexed by piece+6: "kqrbnp.PNBRQK"
// ---------------------------------------------------------------------------
void chessBuildFEN(const BBSChessGame &game, char *buf, size_t bufLen) {
    static const char fenPiece[] = "kqrbnp.PNBRQK"; // piece+6 → char
    size_t pos = 0;

    // Part 1: board (rank 8 down to rank 1)
    for (int rank = 7; rank >= 0 && pos < bufLen - 4; rank--) {
        int empty = 0;
        for (int file = 0; file < 8; file++) {
            int8_t piece = game.board[rank][file];
            if (piece == 0) {
                empty++;
            } else {
                if (empty > 0) { buf[pos++] = '0' + empty; empty = 0; }
                buf[pos++] = fenPiece[piece + 6];
            }
        }
        if (empty > 0) buf[pos++] = '0' + empty;
        if (rank > 0)  buf[pos++] = '/';
    }

    // Part 2: active color
    buf[pos++] = ' ';
    buf[pos++] = (game.toMove == 0) ? 'w' : 'b';

    // Part 3: castling
    buf[pos++] = ' ';
    bool anyCastle = false;
    if (game.castling & 0x01) { buf[pos++] = 'K'; anyCastle = true; }
    if (game.castling & 0x02) { buf[pos++] = 'Q'; anyCastle = true; }
    if (game.castling & 0x04) { buf[pos++] = 'k'; anyCastle = true; }
    if (game.castling & 0x08) { buf[pos++] = 'q'; anyCastle = true; }
    if (!anyCastle)            buf[pos++] = '-';

    // Part 4: en passant target square
    buf[pos++] = ' ';
    if (game.enPassantFile >= 0 && game.enPassantFile < 8) {
        buf[pos++] = 'a' + game.enPassantFile;
        buf[pos++] = (game.toMove == 0) ? '6' : '3';
    } else {
        buf[pos++] = '-';
    }

    buf[pos] = '\0';

    // Parts 5 & 6: halfmove clock and fullmove number
    char clocks[16];
    snprintf(clocks, sizeof(clocks), " %u %u", game.halfMoveClock, game.fullMoveNumber);
    strncat(buf + pos, clocks, bufLen - pos - 1);
}

// ---------------------------------------------------------------------------
// chessAIMove — compute and apply the AI's move
// Returns true if a move was made, false if no legal moves (game over).
// moveOut must be at least 6 bytes.
// ---------------------------------------------------------------------------
bool chessAIMove(BBSChessGame &game, char *moveOut) {
    int depth = CHESS_AI_DEPTHS[game.difficulty < 3 ? game.difficulty : 2];
    s_searchDeadline = millis() + CHESS_AI_TIME_LIMIT_MS;

    bool forWhite = (game.toMove == 0);

    ChessMove moves[MAX_MOVES];
    int count = generateLegal(game, moves, MAX_MOVES);
    if (count == 0) return false;

    int bestVal = forWhite ? -30001 : 30001;
    ChessMove bestMove = moves[0];

    for (int i = 0; i < count; i++) {
        ChessSearchState child;
        child.castling = game.castling;
        child.epFile   = game.enPassantFile;
        child.toMove   = (game.toMove == 0) ? 1 : 0;
        boardApplyRaw(child.board, game.board,
                      moves[i].fr, moves[i].ff, moves[i].tr, moves[i].tf,
                      moves[i].promo, child.castling, child.epFile);

        int val = chessMinimax(child, depth - 1, -30000, 30000, !forWhite);

        bool better = forWhite ? (val > bestVal) : (val < bestVal);
        if (better) {
            bestVal  = val;
            bestMove = moves[i];
        }
        if (millis() >= s_searchDeadline) break;
    }

    // Build the move string and apply it
    char moveStr[6];
    chessMoveToStr(bestMove.fr, bestMove.ff, bestMove.tr, bestMove.tf, bestMove.promo, moveStr);
    if (moveOut) {
        strncpy(moveOut, moveStr, 6);
        moveOut[5] = '\0';
    }

    if (chessApplyMove(game, moveStr)) return true;

    // Best move failed to apply — fall back through all legal moves
    for (int i = 0; i < count; i++) {
        char fallback[6];
        chessMoveToStr(moves[i].fr, moves[i].ff, moves[i].tr, moves[i].tf, moves[i].promo, fallback);
        if (moveOut) { strncpy(moveOut, fallback, 6); moveOut[5] = '\0'; }
        if (chessApplyMove(game, fallback)) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Storage helpers
// ---------------------------------------------------------------------------
static void chessGamePath(uint32_t id, char *path, size_t len) {
    snprintf(path, len, "/bbs/chess/g%lu.bin", (unsigned long)id);
}

bool chessSaveGame(const BBSChessGame &game) {
    chessEnsureDir();
    char path[48];
    chessGamePath(game.id, path, sizeof(path));
    File f = FSCom.open(path, FILE_O_WRITE);
    if (!f) return false;

    f.write((const uint8_t *)&game.id,             sizeof(uint32_t));
    f.write((const uint8_t *)&game.whiteNode,       sizeof(uint32_t));
    f.write((const uint8_t *)&game.blackNode,       sizeof(uint32_t));
    f.write((const uint8_t *)&game.difficulty,      sizeof(uint8_t));
    f.write((const uint8_t *)&game.status,          sizeof(uint8_t));
    f.write((const uint8_t *)&game.toMove,          sizeof(uint8_t));
    f.write((const uint8_t *)&game.castling,        sizeof(uint8_t));
    f.write((const uint8_t *)&game.enPassantFile,   sizeof(int8_t));
    f.write((const uint8_t *)&game._pad,            sizeof(uint8_t));
    f.write((const uint8_t *)&game.halfMoveClock,   sizeof(uint16_t));
    f.write((const uint8_t *)&game.fullMoveNumber,  sizeof(uint16_t));
    f.write((const uint8_t *)&game.lastMove,        sizeof(uint32_t));
    f.write((const uint8_t *)game.lastMoveStr,      sizeof(game.lastMoveStr));
    f.write((const uint8_t *)game.board,            sizeof(ChessBoard));

    f.close();
    return true;
}

bool chessLoadGame(uint32_t id, BBSChessGame &game) {
    char path[48];
    chessGamePath(id, path, sizeof(path));
    File f = FSCom.open(path, FILE_O_READ);
    if (!f) return false;

    f.read((uint8_t *)&game.id,             sizeof(uint32_t));
    f.read((uint8_t *)&game.whiteNode,       sizeof(uint32_t));
    f.read((uint8_t *)&game.blackNode,       sizeof(uint32_t));
    f.read((uint8_t *)&game.difficulty,      sizeof(uint8_t));
    f.read((uint8_t *)&game.status,          sizeof(uint8_t));
    f.read((uint8_t *)&game.toMove,          sizeof(uint8_t));
    f.read((uint8_t *)&game.castling,        sizeof(uint8_t));
    f.read((uint8_t *)&game.enPassantFile,   sizeof(int8_t));
    f.read((uint8_t *)&game._pad,            sizeof(uint8_t));
    f.read((uint8_t *)&game.halfMoveClock,   sizeof(uint16_t));
    f.read((uint8_t *)&game.fullMoveNumber,  sizeof(uint16_t));
    f.read((uint8_t *)&game.lastMove,        sizeof(uint32_t));
    f.read((uint8_t *)game.lastMoveStr,      sizeof(game.lastMoveStr));
    f.read((uint8_t *)game.board,            sizeof(ChessBoard));

    f.close();
    return true;
}

bool chessDeleteGame(uint32_t id) {
    char path[48];
    chessGamePath(id, path, sizeof(path));
    return FSCom.remove(path);
}

// ---------------------------------------------------------------------------
// chessListGames — list up to maxIds game IDs for a given node,
// sorted by lastMove descending. Scans up to 32 files.
// ---------------------------------------------------------------------------
uint32_t chessListGames(uint32_t nodeNum, uint32_t *ids, uint32_t maxIds) {
    if (!ids || maxIds == 0) return 0;

    // Collect candidates from directory listing
    static const uint32_t MAX_SCAN = 32;
    uint32_t scanIds[MAX_SCAN];
    uint32_t scanCount = 0;

    File dir = FSCom.open(BBS_CHESS_DIR, FILE_O_READ);
    if (!dir) return 0;
    BBS_FILE_VAR(f);
    while ((f = dir.openNextFile()) && scanCount < MAX_SCAN) {
        if (!f.isDirectory()) {
            const char *name = f.name();
            // Match files named g<digits>.bin
            if (name[0] == 'g') {
                unsigned long gid = strtoul(name + 1, nullptr, 10);
                if (gid > 0) scanIds[scanCount++] = (uint32_t)gid;
            }
        }
        f.close();
    }
    dir.close();

    // Load each game and filter by nodeNum, track lastMove for sorting
    struct Entry { uint32_t id; uint32_t lastMove; };
    static Entry entries[MAX_SCAN];
    uint32_t entryCount = 0;

    for (uint32_t i = 0; i < scanCount; i++) {
        BBSChessGame game;
        if (!chessLoadGame(scanIds[i], game)) continue;
        if (game.whiteNode == nodeNum || game.blackNode == nodeNum) {
            entries[entryCount++] = {game.id, game.lastMove};
        }
    }

    // Sort descending by lastMove (simple bubble)
    for (uint32_t i = 0; i < entryCount; i++) {
        for (uint32_t j = i + 1; j < entryCount; j++) {
            if (entries[j].lastMove > entries[i].lastMove) {
                Entry tmp = entries[i]; entries[i] = entries[j]; entries[j] = tmp;
            }
        }
    }

    uint32_t result = entryCount < maxIds ? entryCount : maxIds;
    for (uint32_t i = 0; i < result; i++) ids[i] = entries[i].id;
    return result;
}

// ---------------------------------------------------------------------------
// chessNextGameId — reads/writes meta.bin to get a new game ID
// ---------------------------------------------------------------------------
uint32_t chessNextGameId() {
    chessEnsureDir();
    static const uint32_t CHESS_META_MAGIC = 0xCE55CE55;
    uint32_t nextId = 1;

    File f = FSCom.open(BBS_CHESS_META_PATH, FILE_O_READ);
    if (f) {
        uint32_t magic = 0;
        f.read((uint8_t *)&magic, sizeof(uint32_t));
        if (magic == CHESS_META_MAGIC) {
            f.read((uint8_t *)&nextId, sizeof(uint32_t));
        }
        f.close();
    }

    uint32_t allocated = nextId;
    nextId++;

    File fw = FSCom.open(BBS_CHESS_META_PATH, FILE_O_WRITE);
    if (fw) {
        uint32_t magic = CHESS_META_MAGIC;
        fw.write((const uint8_t *)&magic,  sizeof(uint32_t));
        fw.write((const uint8_t *)&nextId, sizeof(uint32_t));
        fw.close();
    }

    return allocated;
}

// ---------------------------------------------------------------------------
// Ratings storage
// ---------------------------------------------------------------------------
BBSChessRating chessGetRating(uint32_t nodeNum) {
    BBSChessRating result;
    memset(&result, 0, sizeof(result));
    result.nodeNum = nodeNum;
    result.rating  = CHESS_DEFAULT_RATING;

    File f = FSCom.open(BBS_CHESS_RATINGS_PATH, FILE_O_READ);
    if (!f) return result;

    BBSChessRating r;
    while (f.read((uint8_t *)&r, sizeof(BBSChessRating)) == sizeof(BBSChessRating)) {
        if (r.nodeNum == nodeNum) {
            f.close();
            return r;
        }
    }
    f.close();
    return result;
}

// Read all ratings from file into buffer. Returns count.
static uint32_t chessLoadAllRatings(BBSChessRating *buf, uint32_t maxCount) {
    File f = FSCom.open(BBS_CHESS_RATINGS_PATH, FILE_O_READ);
    if (!f) return 0;
    uint32_t count = 0;
    while (count < maxCount &&
           f.read((uint8_t *)&buf[count], sizeof(BBSChessRating)) == sizeof(BBSChessRating)) {
        count++;
    }
    f.close();
    return count;
}

// Write all ratings to file (overwrites).
static void chessWriteAllRatings(const BBSChessRating *buf, uint32_t count) {
    chessEnsureDir();
    File f = FSCom.open(BBS_CHESS_RATINGS_PATH, FILE_O_WRITE);
    if (!f) return;
    for (uint32_t i = 0; i < count; i++) {
        f.write((const uint8_t *)&buf[i], sizeof(BBSChessRating));
    }
    f.close();
}

void chessSaveRating(const BBSChessRating &r) {
    static const uint32_t MAX_RATINGS = 128;
    BBSChessRating buf[MAX_RATINGS];
    uint32_t count = chessLoadAllRatings(buf, MAX_RATINGS);

    // Find and update, or append
    bool found = false;
    for (uint32_t i = 0; i < count; i++) {
        if (buf[i].nodeNum == r.nodeNum) {
            buf[i] = r;
            found = true;
            break;
        }
    }
    if (!found && count < MAX_RATINGS) {
        buf[count++] = r;
    }

    chessWriteAllRatings(buf, count);
}

// ---------------------------------------------------------------------------
// chessUpdateRatings — Elo update after a game
// result: 1=white won, 2=black won, 3=draw, 4=stalemate(draw)
// Nodes with value 0 are the computer AI; apply AI's fixed rating instead.
// ---------------------------------------------------------------------------
void chessUpdateRatings(uint32_t whiteNode, uint32_t blackNode, uint8_t result, uint8_t difficulty) {
    // AI nodes (0) don't have persistent ratings; use fixed values
    BBSChessRating white, black;
    bool whiteIsAI = (whiteNode == 0);
    bool blackIsAI = (blackNode == 0);

    if (!whiteIsAI) white = chessGetRating(whiteNode);
    else {
        memset(&white, 0, sizeof(white));
        white.nodeNum = 0;
        white.rating  = CHESS_AI_RATINGS[difficulty < 3 ? difficulty : 2];
    }
    if (!blackIsAI) black = chessGetRating(blackNode);
    else {
        memset(&black, 0, sizeof(black));
        black.nodeNum = 0;
        black.rating  = CHESS_AI_RATINGS[difficulty < 3 ? difficulty : 2];
    }

    // Score from white's perspective
    float scoreWhite, scoreBlack;
    if (result == 1) { scoreWhite = 1.0f; scoreBlack = 0.0f; }
    else if (result == 2) { scoreWhite = 0.0f; scoreBlack = 1.0f; }
    else { scoreWhite = 0.5f; scoreBlack = 0.5f; }

    // Expected scores
    float expWhite = 1.0f / (1.0f + powf(10.0f, ((float)black.rating - (float)white.rating) / 400.0f));
    float expBlack = 1.0f - expWhite;

    int kWhite = (white.gamesPlayed < 20) ? 40 : 20;
    int kBlack = (black.gamesPlayed < 20) ? 40 : 20;

    int newRatingWhite = (int)white.rating + (int)(kWhite * (scoreWhite - expWhite));
    int newRatingBlack = (int)black.rating + (int)(kBlack * (scoreBlack - expBlack));

    // Clamp
    if (newRatingWhite < 100)  newRatingWhite = 100;
    if (newRatingWhite > 3000) newRatingWhite = 3000;
    if (newRatingBlack < 100)  newRatingBlack = 100;
    if (newRatingBlack > 3000) newRatingBlack = 3000;

    if (!whiteIsAI) {
        white.rating = (uint16_t)newRatingWhite;
        white.gamesPlayed++;
        if (result == 1)      white.wins++;
        else if (result == 2) white.losses++;
        else                  white.draws++;
        chessSaveRating(white);
    }
    if (!blackIsAI) {
        black.rating = (uint16_t)newRatingBlack;
        black.gamesPlayed++;
        if (result == 2)      black.wins++;
        else if (result == 1) black.losses++;
        else                  black.draws++;
        chessSaveRating(black);
    }
}

// ---------------------------------------------------------------------------
// chessTopRatings — return the top `max` ratings sorted by rating descending
// ---------------------------------------------------------------------------
uint32_t chessTopRatings(BBSChessRating *out, uint32_t max) {
    if (!out || max == 0) return 0;

    static const uint32_t MAX_RATINGS = 128;
    BBSChessRating buf[MAX_RATINGS];
    uint32_t count = chessLoadAllRatings(buf, MAX_RATINGS);

    // Sort descending by rating (bubble)
    for (uint32_t i = 0; i < count; i++) {
        for (uint32_t j = i + 1; j < count; j++) {
            if (buf[j].rating > buf[i].rating) {
                BBSChessRating tmp = buf[i]; buf[i] = buf[j]; buf[j] = tmp;
            }
        }
    }

    uint32_t result = count < max ? count : max;
    for (uint32_t i = 0; i < result; i++) out[i] = buf[i];
    return result;
}

