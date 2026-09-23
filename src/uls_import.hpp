#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>

#include "models.hpp"

namespace ftxui
{
    class ScreenInteractive;
}

namespace ql
{

    struct AppState;

    // How far along a running FCC ULS import is. Read every render frame by
    // the Settings page, written only by the background import thread -- see
    // StartUlsImport. Embedded by value in AppState (which lives for the
    // whole process), never copied, only ever accessed by pointer.
    enum class UlsImportPhase
    {
        kIdle,
        kDownloading,
        kExtracting,
        kParsing,
        kComplete,
        kFailed,
    };

    struct UlsImportProgress
    {
        std::atomic<bool> running{false};
        std::atomic<UlsImportPhase> phase{UlsImportPhase::kIdle};
        std::atomic<int> percent{0};
        std::atomic<std::int64_t> records_imported{0};

        // last_error is written once (when a run fails) and read occasionally
        // for display; a plain std::string isn't safely readable across
        // threads without synchronization even for "just display", hence the
        // mutex rather than an atomic.
        std::mutex error_mutex;
        std::string last_error;
    };

    // FCC republishes the ULS amateur database weekly; treat a completed
    // import older than this as due for a refresh.
    constexpr std::int64_t kUlsStalenessThresholdSeconds = std::int64_t{7} * 24 * 60 * 60;

    // No-op if progress->running is already true. Otherwise spawns a detached
    // background thread that downloads the current FCC ULS amateur database,
    // extracts it, parses it, and batch-upserts it into `db_path`'s stations
    // table (opening its own Database connection -- never touches the
    // caller's), writing its own final import_runs row when done. Calls
    // screen->PostEvent(ftxui::Event::Custom) periodically so a live
    // percentage repaints while the import runs.
    void StartUlsImport(const std::string& db_path, UlsImportProgress* progress,
                        ftxui::ScreenInteractive* screen);

    // Settings-page display helper: one status line describing the live
    // AppState::uls_import_progress if a run is in progress, otherwise the
    // persisted import_runs row for "uls" (read fresh via AppState::db, not
    // cached, so the text can't go stale while sitting on the page).
    std::string DescribeUlsImportStatus(const AppState* state);

    // Whether a fresh automatic import should be kicked off at startup:
    // never run, a previous run failed, a previous run is stuck "running"
    // (only possible if it crashed, since a clean quit is blocked while an
    // import is running -- see QuitHandler), or the last completed run is
    // older than kUlsStalenessThresholdSeconds.
    bool ShouldAutoStartUlsImport(const std::optional<ImportRunStatus>& status, std::int64_t now);

    // Whether one of StartUlsImport's small, independent auxiliary steps
    // (the ZIP-centroid geocode -- see FetchAndLoadZipCentroids -- or the
    // ZIP-to-county lookup -- see FetchAndLoadZipCounties) needs retrying:
    // it previously failed (e.g. a network hiccup) and hasn't succeeded
    // since. Checked separately from ShouldAutoStartUlsImport because a
    // `uls` row that's "complete" and fresh (<7 days) would otherwise mask a
    // failed `zip_centroids`/`zip_counties` row for up to a week, silently
    // leaving the saved-station form's proximity autocomplete (or
    // ULS-sourced County backfill) unavailable with no way to notice short
    // of manually pressing F3.
    bool ShouldRetryAuxiliaryImport(const std::optional<ImportRunStatus>& status);

}  // namespace ql
