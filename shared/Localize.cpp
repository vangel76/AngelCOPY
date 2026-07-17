#include "Localize.h"

namespace loc {

namespace {

// Index must match enum S exactly.
const wchar_t* kEn[] = {
    // menu
    L"Copy here FAST (AngelCOPY)",
    L"Move here FAST (AngelCOPY)",
    L"Paste FAST (AngelCOPY)",
    L"Delete FAST (AngelCOPY)",
    L"Copy here using robocopy /MT:64",
    L"Move here using robocopy /MT:64",
    L"Paste here using robocopy /MT:64",
    L"Delete permanently (no Recycle Bin), asks first",
    // progress
    L"AngelCOPY \x2014 Copying",
    L"AngelCOPY \x2014 Moving",
    L"AngelCOPY \x2014 Deleting",
    L"Copying items\x2026",
    L"Moving items\x2026",
    L"Deleting permanently\x2026",
    L"%d%%   \x2022   %s   \x2022   %s/s average   \x2022   %llu %s",
    L"Done",
    L"Done \x2014 %llu %s skipped",
    L"Finished with errors",
    L"Cancelled",
    L"Cancelling\x2026",
    L"Close",
    L"Cancel",
    L"%llu %s \x2014 details below:",
    L"%llu %s skipped \x2014 details below:",
    L"Skipped %llu identical %s (%s) \x2014 already up to date at the destination, "
    L"nothing to copy.\r\n",
    L"Skipped %llu existing %s (%s) \x2014 %s.\r\n",
    L"they already existed (\"Skip existing\" was chosen)",
    L"the destination copy was newer (\"Only if newer\" was chosen)",
    L"robocopy reported a failure but printed no error lines (often: destination "
    L"disk full, or access denied).",
    L"Errors:\r\n",
    // conflict
    L"AngelCOPY \x2014 Files already exist",
    L"%llu file at the destination differs from the source.",
    L"%llu files at the destination differ from the source.",
    L"Choose what to do before copying:",
    L"Choose what to do before moving:",
    L"\x2026 and %llu more",
    L"Replace all",
    L"Only if newer",
    L"Skip existing",
    // delete
    L"AngelCOPY \x2014 Delete permanently?",
    L"Permanently delete %llu %s (%s) in %llu %s?",
    L"This does NOT use the Recycle Bin. The files cannot be restored.",
    L"Delete permanently",
    // chart layout
    L"%d%% complete",
    L"Speed: %s/s",
    L"Name: %s",
    L"Time remaining: About %s",
    L"Items remaining: %llu %s (%s)",
    L"calculating\x2026",
    // mirror
    L"Mirror here FAST (AngelCOPY)",
    L"Make this folder an exact copy of the source (robocopy /MT:64, deletes extras)",
    L"AngelCOPY \x2014 Mirroring",
    L"Mirroring items\x2026",
    L"AngelCOPY \x2014 Confirm mirror",
    L"%llu file will be copied or overwritten (%s).",
    L"%llu files will be copied or overwritten (%s).",
    L"%llu item at the destination will be permanently DELETED (%s):",
    L"%llu items at the destination will be permanently DELETED (%s):",
    L"Nothing at the destination needs deleting.",
    L"The destination becomes an exact copy of the source. This cannot be undone.",
    L"Mirror",
};

const wchar_t* kDe[] = {
    // menu
    L"Hierher kopieren FAST (AngelCOPY)",
    L"Hierher verschieben FAST (AngelCOPY)",
    L"Einfügen FAST (AngelCOPY)",
    L"Löschen FAST (AngelCOPY)",
    L"Hierher kopieren mit robocopy /MT:64",
    L"Hierher verschieben mit robocopy /MT:64",
    L"Hier einfügen mit robocopy /MT:64",
    L"Endgültig löschen (kein Papierkorb), fragt vorher",
    // progress
    L"AngelCOPY \x2014 Kopieren",
    L"AngelCOPY \x2014 Verschieben",
    L"AngelCOPY \x2014 Löschen",
    L"Elemente werden kopiert\x2026",
    L"Elemente werden verschoben\x2026",
    L"Wird endgültig gelöscht\x2026",
    L"%d%%   \x2022   %s   \x2022   %s/s Durchschnitt   \x2022   %llu %s",
    L"Fertig",
    L"Fertig \x2014 %llu %s übersprungen",
    L"Mit Fehlern beendet",
    L"Abgebrochen",
    L"Wird abgebrochen\x2026",
    L"Schließen",
    L"Abbrechen",
    L"%llu %s \x2014 Details unten:",
    L"%llu %s übersprungen \x2014 Details unten:",
    L"%llu identische %s (%s) übersprungen \x2014 am Ziel bereits aktuell, "
    L"nichts zu kopieren.\r\n",
    L"%llu vorhandene %s (%s) übersprungen \x2014 %s.\r\n",
    L"sie waren bereits vorhanden (\"Vorhandene überspringen\" gewählt)",
    L"die Datei am Ziel war neuer (\"Nur wenn neuer\" gewählt)",
    L"robocopy meldet einen Fehler, hat aber keine Fehlerzeilen ausgegeben "
    L"(oft: Ziel-Datenträger voll oder Zugriff verweigert).",
    L"Fehler:\r\n",
    // conflict
    L"AngelCOPY \x2014 Dateien existieren bereits",
    L"%llu Datei am Ziel unterscheidet sich von der Quelle.",
    L"%llu Dateien am Ziel unterscheiden sich von der Quelle.",
    L"Vor dem Kopieren wählen:",
    L"Vor dem Verschieben wählen:",
    L"\x2026 und %llu weitere",
    L"Alle ersetzen",
    L"Nur wenn neuer",
    L"Vorhandene überspringen",
    // delete
    L"AngelCOPY \x2014 Endgültig löschen?",
    L"%llu %s (%s) in %llu %s endgültig löschen?",
    L"Kein Papierkorb. Die Dateien können nicht wiederhergestellt werden.",
    L"Endgültig löschen",
    // chart layout
    L"%d%% abgeschlossen",
    L"Geschwindigkeit: %s/s",
    L"Name: %s",
    L"Restdauer: Ungefähr %s",
    L"Verbleibende Elemente: %llu %s (%s)",
    L"wird berechnet\x2026",
    // mirror
    L"Hierher spiegeln FAST (AngelCOPY)",
    L"Ordner wird exakte Kopie der Quelle (robocopy /MT:64, löscht Überzähliges)",
    L"AngelCOPY \x2014 Spiegeln",
    L"Elemente werden gespiegelt\x2026",
    L"AngelCOPY \x2014 Spiegeln bestätigen",
    L"%llu Datei wird kopiert oder überschrieben (%s).",
    L"%llu Dateien werden kopiert oder überschrieben (%s).",
    L"%llu Element am Ziel wird endgültig GELÖSCHT (%s):",
    L"%llu Elemente am Ziel werden endgültig GELÖSCHT (%s):",
    L"Am Ziel muss nichts gelöscht werden.",
    L"Das Ziel wird eine exakte Kopie der Quelle. Das kann nicht rückgängig gemacht werden.",
    L"Spiegeln",
};

static_assert(ARRAYSIZE(kEn) == (size_t)S::COUNT, "English table out of sync with enum S");
static_assert(ARRAYSIZE(kDe) == (size_t)S::COUNT, "German table out of sync with enum S");

bool UseGerman() {
    static int cached = -1;
    if (cached < 0)
        cached = (PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_GERMAN) ? 1 : 0;
    return cached == 1;
}

} // namespace

const wchar_t* T(S id) {
    size_t i = (size_t)id;
    if (i >= (size_t)S::COUNT) return L"";
    return UseGerman() ? kDe[i] : kEn[i];
}

const wchar_t* NounFile(unsigned long long n) {
    if (UseGerman()) return n == 1 ? L"Datei" : L"Dateien";
    return n == 1 ? L"file" : L"files";
}

const wchar_t* NounFoldersIn(unsigned long long n) {
    // German dative plural after "in": "in 1 Ordner", "in 2 Ordnern".
    if (UseGerman()) return n == 1 ? L"Ordner" : L"Ordnern";
    return n == 1 ? L"folder" : L"folders";
}

const wchar_t* NounError(unsigned long long n) {
    if (UseGerman()) return L"Fehler"; // same in both numbers
    return n == 1 ? L"error" : L"errors";
}

} // namespace loc
