#pragma once
// BBSTinyStatus — BBS status display using BBSTinyScreen
//
// Renders a compact status screen on SSD1306 OLED without
// depending on Meshtastic's 150KB Screen framework.
//
// Layout (128x64, 8 lines of 21 chars):
//   Line 0: [TinyBBS    nodes:12] (inverted header)
//   Line 1: ─────────────────────
//   Line 2: Bul:12  Mail:5  QSL:3
//   Line 3: Last: GoatNode  2m
//   Line 4: ─────────────────────
//   Line 5: RX:847 TX:312 Air:42%
//   Line 6: Uptime: 3h 22m
//   Line 7: ExtFlash 1.8MB free

#ifdef NRF52_SERIES

#include "BBSTinyScreen.h"
#include "BBSStorage.h"

class BBSTinyStatus {
  private:
    BBSTinyScreen screen_;
    bool active_ = false;

    // Cached stats (updated by refresh(), not every frame)
    uint32_t bulletins_ = 0;
    uint32_t mail_ = 0;
    uint32_t qsl_ = 0;
    uint32_t sessions_ = 0;
    uint32_t lastMsgTime_ = 0;
    char     lastMsgFrom_[12] = {0};
    uint32_t lastRefresh_ = 0;

    static constexpr uint32_t REFRESH_INTERVAL_S = 5;

  public:
    bool begin() {
        active_ = screen_.begin();
        return active_;
    }

    bool isActive() const { return active_; }

    // Call from handleReceived to track last message
    void notifyMessage(uint32_t fromNode, const char *fromName, uint32_t timestamp) {
        lastMsgTime_ = timestamp;
        if (fromName) {
            strncpy(lastMsgFrom_, fromName, sizeof(lastMsgFrom_) - 1);
            lastMsgFrom_[sizeof(lastMsgFrom_) - 1] = '\0';
        } else {
            snprintf(lastMsgFrom_, sizeof(lastMsgFrom_), "!%08x", fromNode);
        }
    }

    // Call from runOnce — updates screen periodically
    void refresh(uint32_t now, BBSStorage *storage, uint8_t activeSessions,
                 uint32_t rxCount, uint32_t txCount, float airUtil,
                 uint16_t nodesOnline, uint32_t freeBytes) {

        if (!active_) return;
        if (now - lastRefresh_ < REFRESH_INTERVAL_S) return;
        lastRefresh_ = now;

        // Update stats from storage
        if (storage) {
            BBSStats stats = storage->getStats();
            bulletins_ = stats.totalBulletins;
            qsl_       = stats.totalQSLItems;
        }
        sessions_ = activeSessions;

        // Build the display
        screen_.clear();

        // Line 0: header (inverted)
        screen_.printfAt(0, 0, "TinyBBS    nodes:%-3u", nodesOnline);
        screen_.invertRow(0);

        // Line 1: separator
        screen_.hline(15);

        // Line 2: content counts
        screen_.printfAt(0, 2, "Bul:%-3u Mail QSL:%-2u", bulletins_, qsl_);

        // Line 3: last message
        if (lastMsgTime_ > 0 && now > lastMsgTime_) {
            uint32_t ago = now - lastMsgTime_;
            if (ago < 60)
                screen_.printfAt(0, 3, "Last:%-10s %us", lastMsgFrom_, ago);
            else if (ago < 3600)
                screen_.printfAt(0, 3, "Last:%-10s %um", lastMsgFrom_, ago / 60);
            else
                screen_.printfAt(0, 3, "Last:%-10s %uh", lastMsgFrom_, ago / 3600);
        } else {
            screen_.printAt(0, 3, "Last: --");
        }

        // Line 4: separator
        screen_.hline(39);

        // Line 5: radio stats
        screen_.printfAt(0, 5, "RX:%-4u TX:%-4u %2u%%",
                         rxCount, txCount, (uint8_t)airUtil);

        // Line 6: uptime
        uint32_t up = now;  // seconds since boot (approx)
        if (up >= 3600)
            screen_.printfAt(0, 6, "Up:%uh%um  Sess:%u", up / 3600, (up % 3600) / 60, sessions_);
        else
            screen_.printfAt(0, 6, "Up:%um     Sess:%u", up / 60, sessions_);

        // Line 7: storage
        if (freeBytes > 1024 * 1024)
            screen_.printfAt(0, 7, "ExtFlash %.1fMB free", (float)freeBytes / (1024 * 1024));
        else if (freeBytes > 1024)
            screen_.printfAt(0, 7, "Flash %uKB free", freeBytes / 1024);
        else
            screen_.printfAt(0, 7, "Flash %uB free", freeBytes);

        screen_.display();
    }
};

#endif // NRF52_SERIES
