#pragma once
#include <windows.h>

// Shared by AngelCopyShell.dll and AngelCopyRunner.exe. Language is picked once
// from the system UI language: German if that is German, English otherwise.
//
// Rule for translators/maintainers: for a given id, EVERY language must consume
// the SAME format arguments in the SAME order. Word order may differ, argument
// order may not.

namespace loc {

enum class S {
    // ---- shell menu entries ----
    // Pattern: "<verb> FAST (AngelCOPY)". Verb first, because the shell's own
    // items lead with the verb too ("Hierher kopieren") — that is what the eye
    // scans. FAST also keeps us distinguishable from the native entry sitting
    // right below.
    MenuCopyHere, MenuMoveHere, MenuPaste, MenuDelete,
    HelpCopyHere, HelpMoveHere, HelpPaste, HelpDelete,

    // ---- progress dialog ----
    CapCopying, CapMoving, CapDeleting,
    HeadCopying, HeadMoving, HeadDeleting,
    StatsDone,         // pct, total, rate, totalFiles, noun
    TitleDone,
    TitleDoneSkipped,  // count, noun
    TitleErrors, TitleCancelled, TitleCancelling,
    BtnClose, BtnCancel,
    HdrErrors,         // count, noun
    HdrSkipped,        // count, noun
    SkipIdentical,     // count, noun, bytes
    SkipPolicy,        // count, noun, bytes, reason
    WhySkipExisting, WhyOnlyNewer,
    NoErrorLines, ErrorsLabel,

    // ---- conflict dialog ----
    ConflictCaption,
    // Singular and plural are separate strings, not a noun swap: the verb has to
    // agree with the count in both languages ("1 file differs" / "2 files
    // differ", "1 Datei unterscheidet sich" / "2 Dateien unterscheiden sich").
    ConflictHeadOne,   // count
    ConflictHeadMany,  // count
    ConflictSubCopy, ConflictSubMove,
    ConflictMore,      // count
    BtnReplaceAll, BtnOnlyNewer, BtnSkipExisting,

    // ---- delete confirmation ----
    DeleteCaption,
    DeleteHead,        // files, nounFile, bytes, dirs, nounFolder
    DeleteWarn, BtnDeletePerm,

    // ---- chart layout ----
    // Appended at the end on purpose: the tables are positional, and inserting
    // in the middle silently shifts every following string.
    HeadPercent,       // pct
    ChartSpeed,        // rate
    LineName,          // path
    LineEta,           // eta
    LineRemaining,     // count, noun, bytes
    EtaCalculating,

    // ---- mirror ("Spiegeln": one-way sync, source wins, extras deleted) ----
    // Appended at the end (positional tables). One/Many pairs are separate
    // strings because the verb must agree with the count in both languages.
    MenuSyncHere, HelpSyncHere,
    CapSyncing, HeadSyncing,
    SyncCaption,
    SyncCopyOne,  SyncCopyMany,  // count, bytes
    SyncDelOne,   SyncDelMany,   // count, bytes
    SyncDelNone,
    SyncWarn,
    BtnSync,

    // ---- low-disk-space warning ----
    SpaceCaption,
    SpaceHead,         // needed, free  (both %s, pre-formatted)
    SpaceShort,        // shortfall %s
    SpaceHint,
    BtnProceedAnyway,

    COUNT
};

// Localized string for `id`.
const wchar_t* T(S id);

// Correctly inflected nouns for the %s slots above.
const wchar_t* NounFile(unsigned long long n);
const wchar_t* NounError(unsigned long long n);

// Specifically the noun in the "in %llu <folders>" slot of DeleteHead. German
// needs the dative plural there ("in 2 Ordnern", not "in 2 Ordner"), so this
// cannot be a generic folder noun — do not reuse it elsewhere.
const wchar_t* NounFoldersIn(unsigned long long n);

} // namespace loc
