#pragma once
#include <cstdint>
#include <cstddef>

#define BBS_CHESS_DIR          "/bbs/chess"
#define BBS_CHESS_META_PATH    "/bbs/chess/meta.bin"
#define BBS_CHESS_RATINGS_PATH "/bbs/chess/ratings.bin"

// Piece encoding: 0=empty, 1=P,2=N,3=B,4=R,5=Q,6=K (positive=white, negative=black)
typedef int8_t ChessBoard[8][8]; // [rank][file], rank 0 = white's back rank (rank 1)

struct BBSChessGame {
    uint32_t id;
    uint32_t whiteNode;      // 0 = computer
    uint32_t blackNode;      // 0 = computer
    uint8_t  difficulty;     // 0=Easy,1=Medium,2=Hard
    uint8_t  status;         // 0=active,1=white won,2=black won,3=draw,4=stalemate
    uint8_t  toMove;         // 0=white,1=black
    uint8_t  castling;       // bits: 0=WK,1=WQ,2=BK,3=BQ
    int8_t   enPassantFile;  // -1 if none, 0-7
    uint8_t  _pad;
    uint16_t halfMoveClock;
    uint16_t fullMoveNumber;
    uint32_t lastMove;       // unix timestamp
    char     lastMoveStr[6]; // "e2e4\0" or "e7e8q\0"
    ChessBoard board;        // 64 bytes -- always last
};

struct BBSChessRating {
    uint32_t nodeNum;
    uint16_t rating;
    uint16_t gamesPlayed;
    uint16_t wins;
    uint16_t losses;
    uint16_t draws;
};

static const uint16_t CHESS_AI_RATINGS[3]    = {800, 1200, 1600};
static const int      CHESS_AI_DEPTHS[3]     = {1, 3, 5};
static const uint16_t CHESS_DEFAULT_RATING   = 1200;
static const uint32_t CHESS_AI_TIME_LIMIT_MS = 8000;

extern const char *CHESS_PIECE_EMOJI[13]; // indexed by piece+6
extern const char *CHESS_SQ_WHITE;
extern const char *CHESS_SQ_BLACK;

void     chessEnsureDir();
void     chessBoardInit(ChessBoard b);
bool     chessApplyMove(BBSChessGame &game, const char *move);
bool     chessAIMove(BBSChessGame &game, char *moveOut);
void     chessBuildBoard(const BBSChessGame &game, char *buf, size_t bufLen, bool whiteAtBottom);
void     chessBuildFEN(const BBSChessGame &game, char *buf, size_t bufLen);
bool     chessIsInCheck(const ChessBoard b, bool whiteKing);
bool     chessHasLegalMoves(const BBSChessGame &game);
uint8_t  chessCheckTermination(BBSChessGame &game);
bool     chessParseMove(const char *move, int *fr, int *ff, int *tr, int *tf, int8_t *promo);
void     chessMoveToStr(int fr, int ff, int tr, int tf, int8_t promo, char *out);
bool     chessSaveGame(const BBSChessGame &game);
bool     chessLoadGame(uint32_t id, BBSChessGame &game);
bool     chessDeleteGame(uint32_t id);
uint32_t chessListGames(uint32_t nodeNum, uint32_t *ids, uint32_t maxIds);
uint32_t chessNextGameId();
BBSChessRating chessGetRating(uint32_t nodeNum);
void     chessSaveRating(const BBSChessRating &r);
void     chessUpdateRatings(uint32_t whiteNode, uint32_t blackNode, uint8_t result, uint8_t difficulty);
uint32_t chessTopRatings(BBSChessRating *out, uint32_t max);
