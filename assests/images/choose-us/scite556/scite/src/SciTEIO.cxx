// SciTE - Scintilla based Text Editor
/** @file SciTEIO.cxx
 ** Manage input and output with the system.
 **/
// Copyright 1998-2010 by Neil Hodgson <neilh@scintilla.org>
// The License.txt file describes the conditions under which this software may be distributed.

#include <cstddef>
#include <cstdlib>
// Older versions of GNU stdint.h require this definition to be able to see INT32_MAX
#define __STDC_LIMIT_MACROS
#include <cstdint>
#include <cassert>
#include <cstring>
#include <cstdio>
#include <ctime>

#include <compare>
#include <tuple>
#include <string>
#include <string_view>
#include <vector>
#include <map>
#include <set>
#include <optional>
#include <algorithm>
#include <memory>
#include <chrono>
#include <atomic>
#include <mutex>

#include <fcntl.h>

#include "ILoader.h"

#include "ScintillaTypes.h"
#include "ScintillaCall.h"

#include "GUI.h"
#include "ScintillaWindow.h"

#include "StringList.h"
#include "StringHelpers.h"
#include "FilePath.h"
#include "StyleDefinition.h"
#include "PropSetFile.h"
#include "StyleWriter.h"
#include "Extender.h"
#include "SciTE.h"
#include "JobQueue.h"
#include "Cookie.h"
#include "Worker.h"
#include "Utf8_16.h"
#include "FileWorker.h"
#include "MatchMarker.h"
#include "Searcher.h"
#include "SciTEBase.h"

#if defined(GTK)
const GUI::gui_char propUserFileName[] = GUI_TEXT(".SciTEUser.properties");
#elif defined(__APPLE__)
const GUI::gui_char propUserFileName[] = GUI_TEXT("SciTEUser.properties");
#else
// Windows
const GUI::gui_char propUserFileName[] = GUI_TEXT("SciTEUser.properties");
#endif
const GUI::gui_char propGlobalFileName[] = GUI_TEXT("SciTEGlobal.properties");
const GUI::gui_char propAbbrevFileName[] = GUI_TEXT("abbrev.properties");

void SciTEBase::SetFileName(const FilePath &openName, bool fixCase) {
	if (openName.AsInternal()[0] == '\"') {
		// openName is surrounded by double quotes
		GUI::gui_string pathCopy = openName.AsInternal();
		pathCopy = pathCopy.substr(1, pathCopy.size() - 2);
		filePath.Set(pathCopy);
	} else {
		filePath.Set(openName);
	}

	// Break fullPath into directory and file name using working directory for relative paths
	if (!filePath.IsAbsolute()) {
		// Relative path. Since we ran AbsolutePath, we probably are here because fullPath is empty.
		filePath.SetDirectory(filePath.Directory());
	}

	if (fixCase) {
		filePath.FixName();
	}

	ReadLocalPropFile();

	SetWindowName();
	if (buffers.buffers.size() > 0)
		CurrentBuffer()->file.Set(filePath);
}

// See if path exists.
// If path is not absolute, it is combined with dir.
// If resultPath is not NULL, it receives the absolute path if it exists.
bool SciTEBase::Exists(const GUI::gui_char *dir, const GUI::gui_char *path, FilePath *resultPath) {
	FilePath copy(path);
	if (!copy.IsAbsolute() && dir) {
		copy.SetDirectory(dir);
	}
	if (!copy.Exists())
		return false;
	if (resultPath) {
		resultPath->Set(copy.AbsolutePath());
	}
	return true;
}

void SciTEBase::CountLineEnds(int &linesCR, int &linesLF, int &linesCRLF) {
	linesCR = 0;
	linesLF = 0;
	linesCRLF = 0;
	const SA::Position lengthDoc = LengthDocument();
	char chPrev = ' ';
	TextReader acc(wEditor);
	char chNext = acc.SafeGetCharAt(0);
	for (SA::Position i = 0; i < lengthDoc; i++) {
		const char ch = chNext;
		chNext = acc.SafeGetCharAt(i + 1);
		if (ch == '\r') {
			if (chNext == '\n')
				linesCRLF++;
			else
				linesCR++;
		} else if (ch == '\n') {
			if (chPrev != '\r') {
				linesLF++;
			}
		} else if (i > 1000000) {
			return;
		}
		chPrev = ch;
	}
}

void SciTEBase::DiscoverEOLSetting() {
	SetEol();
	if (props.GetInt("eol.auto")) {
		int linesCR;
		int linesLF;
		int linesCRLF;
		CountLineEnds(linesCR, linesLF, linesCRLF);
		if (((linesLF >= linesCR) && (linesLF > linesCRLF)) || ((linesLF > linesCR) && (linesLF >= linesCRLF)))
			wEditor.SetEOLMode(SA::EndOfLine::Lf);
		else if (((linesCR >= linesLF) && (linesCR > linesCRLF)) || ((linesCR > linesLF) && (linesCR >= linesCRLF)))
			wEditor.SetEOLMode(SA::EndOfLine::Cr);
		else if (((linesCRLF >= linesLF) && (linesCRLF > linesCR)) || ((linesCRLF > linesLF) && (linesCRLF >= linesCR)))
			wEditor.SetEOLMode(SA::EndOfLine::CrLf);
	}
}

// Look inside the first line for a #! clue regarding the language
std::string SciTEBase::DiscoverLanguage() {
	constexpr SA::Position oneK = 1024;
	const SA::Position length = std::min(LengthDocument(), 64 * oneK);
	std::string buf = wEditor.StringOfRange(SA::Span(0, length));
	std::string languageOverride;
	std::string_view line = ExtractLine(buf);
	if (line.starts_with("<?xml")) {
		languageOverride = "xml";
	} else if (line.starts_with("#!")) {
		line.remove_prefix(2);
		std::string l1(line);
		std::ranges::replace(l1, '\\', ' ');
		std::ranges::replace(l1, '/', ' ');
		std::ranges::replace(l1, '\t', ' ');
		Substitute(l1, "  ", " ");
		Substitute(l1, "  ", " ");
		Substitute(l1, "  ", " ");
		::Remove(l1, std::string("\r"));
		::Remove(l1, std::string("\n"));
		if (l1.starts_with(" ")) {
			l1 = l1.substr(1);
		}
		std::ranges::replace(l1, ' ', '\0');
		l1.append(1, '\0');
		const char *word = l1.c_str();
		while (*word) {
			std::string propShBang("shbang.");
			propShBang.append(word);
			std::string langShBang = props.GetExpandedString(propShBang);
			if (langShBang.length()) {
				languageOverride = langShBang;
			}
			word += strlen(word) + 1;
		}
	}
	if (languageOverride.length()) {
		languageOverride.insert(0, "x.");
	}
	return languageOverride;
}

void SciTEBase::DiscoverIndentSetting() {
	const SA::Position lengthDoc = std::min<SA::Position>(LengthDocument(), 1000000);
	TextReader acc(wEditor);
	bool newline = true;
	int indent = 0; // current line indentation
	int tabSizes[9] = { 0, 0, 0, 0, 0, 0, 0, 0, 0 }; // number of lines with corresponding indentation (index 0 - tab)
	int prevIndent = 0; // previous line indentation
	int prevTabSize = -1; // previous line tab size
	for (int i = 0; i < lengthDoc; i++) {
		const char ch = acc[i];
		if (ch == '\r' || ch == '\n') {
			indent = 0;
			newline = true;
		} else if (newline && ch == ' ') {
			indent++;
		} else if (newline) {
			if (indent) {
				if (indent == prevIndent && prevTabSize != -1) {
					tabSizes[prevTabSize]++;
				} else if (indent > prevIndent && prevIndent != -1) {
					if (indent - prevIndent <= 8) {
						prevTabSize = indent - prevIndent;
						tabSizes[prevTabSize]++;
					} else {
						prevTabSize = -1;
					}
				}
				prevIndent = indent;
			} else if (ch == '\t') {
				tabSizes[0]++;
				prevIndent = -1;
			} else {
				prevIndent = 0;
			}
			newline = false;
		}
	}
	// maximum non-zero indent
	int topTabSize = -1;
	for (int j = 0; j <= 8; j++) {
		if (tabSizes[j] && (topTabSize == -1 || tabSizes[j] > tabSizes[topTabSize])) {
			topTabSize = j;
		}
	}
	// set indentation
	if (topTabSize == 0) {
		wEditor.SetUseTabs(true);
		wEditor.SetIndent(wEditor.TabWidth());
	} else if (topTabSize != -1) {
		wEditor.SetUseTabs(false);
		wEditor.SetIndent(topTabSize);
	}
}

namespace {

SA::DocumentOption LoadingOptions(const PropSetFile &props, const long long fileSize) {
	SA::DocumentOption docOptions = SA::DocumentOption::Default;

	const long long sizeLarge = props.GetLongLong("file.size.large");
	if (sizeLarge && (fileSize > sizeLarge))
		docOptions = SA::DocumentOption::TextLarge;

	const long long sizeNoStyles = props.GetLongLong("file.size.no.styles");
	if (sizeNoStyles && (fileSize > sizeNoStyles))
		docOptions = docOptions | SA::DocumentOption::StylesNone;

	return docOptions;
}

void AddText(GUI::ScintillaWindow &wDestination, std::string_view sv) {
	wDestination.AddText(sv.size(), sv.data());
}

}

void SciTEBase::OpenCurrentFile(const long long fileSize, bool suppressMessage, bool asynchronous) {
	// Allocate a bit extra to allow minor edits without reallocation.
	const long long fileAllocationSize = fileSize + 1000;
	if (fileAllocationSize >= PTRDIFF_MAX || fileSize < 0) {
		if (!suppressMessage) {
			GUI::gui_string msg = LocaliseMessage("Could not open file '^0'.", filePath.AsInternal());
			WindowMessageBox(wSciTE, msg);
		}
		return;
	}

	if (CurrentBuffer()->pFileWorker) {
		// Already performing an asynchronous load or save so do not restart load
		if (!suppressMessage) {
			GUI::gui_string msg = LocaliseMessage("Could not open file '^0'.", filePath.AsInternal());
			WindowMessageBox(wSciTE, msg);
		}
		return;
	}

	FILE *fp = filePath.Open(fileRead);
	if (!fp) {
		if (!suppressMessage) {
			GUI::gui_string msg = LocaliseMessage("Could not open file '^0'.", filePath.AsInternal());
			WindowMessageBox(wSciTE, msg);
		}
		if (!wEditor.UndoCollection()) {
			wEditor.SetUndoCollection(true);
		}
		return;
	}

	const SA::Position bufferSize = static_cast<SA::Position>(fileAllocationSize);

	CurrentBuffer()->SetTimeFromFile();

	CurrentBuffer()->lifeState = Buffer::LifeState::reading;
	if (asynchronous) {
		wEditor.ClearAll();
		// Turn grey while loading
		wEditor.StyleSetBack(StyleDefault, 0xEEEEEE);
		wEditor.SetReadOnly(true);
		assert(CurrentBufferConst()->pFileWorker == nullptr);
		Scintilla::ILoader *pdocLoad = nullptr;
		try {
			const SA::DocumentOption docOptions = LoadingOptions(props, fileSize);
			pdocLoad = static_cast<Scintilla::ILoader *>(
					   wEditor.CreateLoader(bufferSize, docOptions));
		} catch (...) {
			wEditor.SetStatus(SA::Status::Ok);
			return;
		}
		CurrentBuffer()->pFileWorker = std::make_unique<FileLoader>(this, pdocLoad, filePath, static_cast<size_t>(fileSize), fp);
		CurrentBuffer()->pFileWorker->sleepTime = props.GetInt("asynchronous.sleep");
		PerformOnNewThread(CurrentBuffer()->pFileWorker.get());
	} else {
		std::unique_ptr<Utf8_16::Reader> convert = Utf8_16::Reader::Allocate();
		{
			UndoBlock ub(wEditor);	// Group together clear and insert
			wEditor.ClearAll();
			wEditor.Allocate(bufferSize);
			std::vector<char> data(blockSize);
			size_t lenFile = fread(data.data(), 1, data.size(), fp);
			while (lenFile > 0) {
				const std::string_view dataBlock = convert->convert(std::string_view(data.data(), lenFile));
				AddText(wEditor, dataBlock);
				lenFile = fread(data.data(), 1, data.size(), fp);
			}
			fclose(fp);
			// Handle case where convert is holding a lead surrogate but no more data
			const std::string_view dataTrail = convert->convert("");
			AddText(wEditor, dataTrail);
		}

		CurrentBuffer()->unicodeMode = convert->getEncoding();

		CompleteOpen(OpenCompletion::synchronous);
	}
}

void SciTEBase::TextRead(FileWorker *pFileWorker) {
	FileLoader *pFileLoader = dynamic_cast<FileLoader *>(pFileWorker);
	const BufferIndex iBuffer = buffers.GetDocumentByWorker(pFileLoader);
	// May not be found if load cancelled
	if ((iBuffer >= 0) && pFileLoader) {
		buffers.buffers[iBuffer].unicodeMode = pFileLoader->unicodeMode;
		buffers.buffers[iBuffer].lifeState = Buffer::LifeState::readAll;
		if (pFileLoader->err) {
			GUI::gui_string msg = LocaliseMessage("Could not open file '^0'.", pFileLoader->path.AsInternal());
			WindowMessageBox(wSciTE, msg);
			// Should refuse to save when failure occurs
			buffers.buffers[iBuffer].lifeState = Buffer::LifeState::empty;
		}
		// Switch documents
		SA::IDocumentEditable *pdocLoading = static_cast<SA::IDocumentEditable *>(
			pFileLoader->pLoader->ConvertToDocument());
		pFileLoader->pLoader = nullptr;
		SwitchDocumentAt(iBuffer, pdocLoading);
		if (iBuffer == buffers.Current()) {
			CompleteOpen(OpenCompletion::completeCurrent);
			if (extender)
				extender->OnOpen(buffers.buffers[iBuffer].file.AsUTF8().c_str());
			RestoreState(buffers.buffers[iBuffer], true);
			DisplayAround(buffers.buffers[iBuffer].file.filePosition);
			wEditor.ScrollCaret();
		}
	}
}

void SciTEBase::PerformDeferredTasks() {
	if (CurrentBuffer()->FinishSave()) {
		wEditor.SetSavePoint();
		wEditor.SetReadOnly(CurrentBuffer()->isReadOnly);
	}
}

void SciTEBase::CompleteOpen(OpenCompletion oc) {
	wEditor.SetReadOnly(CurrentBuffer()->isReadOnly);

	if (oc != OpenCompletion::synchronous) {
		ReadProperties();
	}

	if (language == "" || language == "null") {
		std::string languageOverride = DiscoverLanguage();
		if (languageOverride.length()) {
			CurrentBuffer()->overrideExtension = languageOverride;
			CurrentBuffer()->lifeState = Buffer::LifeState::opened;
			ReadProperties();
			SetIndentSettings();
		}
	}

	if (oc != OpenCompletion::synchronous) {
		SetIndentSettings();
		SetEol();
		UpdateBuffersCurrent();
		SizeSubWindows();
	}

	if (CurrentBuffer()->unicodeMode != UniMode::uni8Bit) {
		// Override the code page if Unicode
		codePage = SA::CpUtf8;
	} else {
		codePage = props.GetInt("code.page");
	}
	wEditor.SetCodePage(codePage);

	DiscoverEOLSetting();

	if (props.GetInt("indent.auto")) {
		DiscoverIndentSetting();
	}

	if (!wEditor.UndoCollection()) {
		wEditor.SetUndoCollection(true);
		wEditor.SetSavePoint();
		wEditor.SetChangeHistory(static_cast<SA::ChangeHistoryOption>(props.GetInt("change.history")));
	} else {
		wEditor.SetSavePoint();
	}
	if (props.GetInt("fold.on.open") > 0) {
		FoldAll();
	}
	wEditor.GotoPos(0);

	if (FilterShowing()) {
		FilterAll(true);
	}

	CurrentBuffer()->CompleteLoading();

	Redraw();
}

void SciTEBase::TextWritten(FileWorker *pFileWorker) {
	const FileStorer *pFileStorer = dynamic_cast<const FileStorer *>(pFileWorker);
	assert(pFileStorer);
	if (!pFileStorer) {
		return;
	}
	const BufferIndex iBuffer = buffers.GetDocumentByWorker(pFileStorer);

	FilePath pathSaved = pFileStorer->path;
	const int errSaved = pFileStorer->err;
	const bool cancelledSaved = pFileStorer->Cancelling();

	// May not be found if save cancelled or buffer closed
	if (iBuffer >= 0) {
		// Complete and release
		buffers.buffers[iBuffer].CompleteStoring();
		if (errSaved || cancelledSaved) {
			// Background save failed (possibly out-of-space) so resurrect the
			// buffer so can be saved to another disk or retried after making room.
			buffers.SetVisible(iBuffer, true);
			SetBuffersMenu();
			if (iBuffer == buffers.Current()) {
				wEditor.SetReadOnly(CurrentBuffer()->isReadOnly);
			}
		} else {
			if (!buffers.GetVisible(iBuffer)) {
				buffers.RemoveInvisible(iBuffer);
			}
			if (iBuffer == buffers.Current()) {
				wEditor.SetReadOnly(CurrentBuffer()->isReadOnly);
				if (pathSaved.SameNameAs(CurrentBuffer()->file)) {
					wEditor.SetSavePoint();
				}
				if (extender)
					extender->OnSave(buffers.buffers[iBuffer].file.AsUTF8().c_str());
			} else {
				// Need to make writable and set save point when next receive focus.
				buffers.buffers[iBuffer].ScheduleFinishSave();
				SetBuffersMenu();
			}
		}
	} else {
		GUI::gui_string msg = LocaliseMessage("Could not find buffer '^0'.", pathSaved.AsInternal());
		WindowMessageBox(wSciTE, msg);
	}

	if (errSaved) {
		FailedSaveMessageBox(pathSaved);
	}

	if (IsPropertiesFile(pathSaved)) {
		ReloadProperties();
	}
	UpdateStatusBar(true);
	if (!jobQueue.executing && (jobQueue.HasCommandToRun())) {
		Execute();
	}
	if (quitting && !buffers.SavingInBackground()) {
		QuitProgram();
	}
}

void SciTEBase::UpdateProgress(Worker *) {
	BackgroundActivities bgActivities = buffers.CountBackgroundActivities();
	const int countBoth = bgActivities.loaders + bgActivities.storers;
	if (countBoth == 0) {
		// Should hide UI
		ShowBackgroundProgress(GUI_TEXT(""), 0, 0);
	} else {
		GUI::gui_string prog;
		if (countBoth == 1) {
			prog += LocaliseMessage(bgActivities.loaders ? "Opening '^0'" : "Saving '^0'",
						bgActivities.fileNameLast.c_str());
		} else {
			if (bgActivities.loaders) {
				prog += LocaliseMessage("Opening ^0 files ", GUI::StringFromInteger(bgActivities.loaders).c_str());
			}
			if (bgActivities.storers) {
				prog += LocaliseMessage("Saving ^0 files ", GUI::StringFromInteger(bgActivities.storers).c_str());
			}
		}
		ShowBackgroundProgress(prog, bgActivities.totalWork, bgActivities.totalProgress);
	}
}

bool SciTEBase::PreOpenCheck(const GUI::gui_string &) {
	return false;
}

bool SciTEBase::Open(const FilePath &file, OpenFlags of) {
	InitialiseBuffers();

	FilePath absPath = file.AbsolutePath();
	if (!absPath.IsUntitled() && absPath.IsDirectory()) {
		GUI::gui_string msg = LocaliseMessage("Path '^0' is a directory so can not be opened.",
						      absPath.AsInternal());
		WindowMessageBox(wSciTE, msg);
		return false;
	}

	const BufferIndex index = buffers.GetDocumentByName(absPath);
	if (index >= 0) {
		buffers.SetVisible(index, true);
		SetDocumentAt(index);
		RemoveFileFromStack(absPath);
		DeleteFileStackMenu();
		SetFileStackMenu();
		// If not forcing reload or currently busy with load or save, just rotate into view
		if ((!(of & ofForceLoad)) || (CurrentBufferConst()->pFileWorker))
			return true;
	}
	// See if we can have a buffer for the file to open
	if (!CanMakeRoom(!(of & ofNoSaveIfDirty))) {
		return false;
	}

	const long long fileSize = absPath.IsUntitled() ? 0 : absPath.GetFileLength();
#if !defined(_WIN64)
	// The #if is just to prevent a warning from Coverity on 64-bit Win32
	if (fileSize > INTPTR_MAX) {
		const GUI::gui_string sSize = GUI::StringFromLongLong(fileSize);
		const GUI::gui_string msg = LocaliseMessage("File '^0' is ^1 bytes long, "
					    "larger than 2GB which is the largest SciTE can open.",
					    absPath.AsInternal(), sSize.c_str());
		WindowMessageBox(wSciTE, msg, mbsIconWarning);
		return false;
	}
#endif
	if (fileSize > 0) {
		// Real file, not empty buffer
		const long long maxSize = props.GetLongLong("max.file.size", 2000000000LL);
		if (maxSize > 0 && fileSize > maxSize) {
			const GUI::gui_string sSize = GUI::StringFromLongLong(fileSize);
			const GUI::gui_string sMaxSize = GUI::StringFromLongLong(maxSize);
			const GUI::gui_string msg = LocaliseMessage("File '^0' is ^1 bytes long,\n"
						    "larger than the ^2 bytes limit set in the properties.\n"
						    "Do you still want to open it?",
						    absPath.AsInternal(), sSize.c_str(), sMaxSize.c_str());
			const MessageBoxChoice answer = WindowMessageBox(wSciTE, msg, mbsYesNo | mbsIconWarning);
			if (answer != MessageBoxChoice::yes) {
				return false;
			}
		}
	}

	if (buffers.size() == buffers.length) {
		AddFileToStack(RecentFile(filePath, GetFilePosition()));
		ClearDocument();
		CurrentBuffer()->lifeState = Buffer::LifeState::opened;
		if (extender)
			extender->InitBuffer(buffers.Current());
	} else {
		if (index < 0 || !(of & ofForceLoad)) { // No new buffer, already opened
			New();
		}
	}

	assert(CurrentBufferConst()->pFileWorker == nullptr);
	SetFileName(absPath);

	propsDiscovered.Clear();
	if (propsUser.GetInt("discover.properties")) {
		std::string discoveryScript = props.GetExpandedString("command.discover.properties");
		if (discoveryScript.length()) {
			std::string propertiesText = CommandExecute(GUI::StringFromUTF8(discoveryScript).c_str(),
						     absPath.Directory().AsInternal());
			if (propertiesText.size()) {
				propsDiscovered.ReadFromMemory(propertiesText, absPath.Directory(), filter, nullptr, 0);
			}
		}
	}
	CurrentBuffer()->props = propsDiscovered;
	CurrentBuffer()->overrideExtension = "";
	ReadProperties();
	SetIndentSettings();
	SetEol();
	UpdateBuffersCurrent();
	SizeSubWindows();

	bool asynchronous = false;
	if (!filePath.IsUntitled()) {
		wEditor.SetReadOnly(false);
		wEditor.Cancel();

		bool allowUndoLoad = of & ofPreserveUndo;

		asynchronous = (fileSize > props.GetInt("background.open.size", -1)) &&
			!(of & (ofPreserveUndo | ofSynchronous));
		const SA::DocumentOption loadingOptions = LoadingOptions(props, fileSize);
		if (!asynchronous && loadingOptions != wEditor.DocumentOptions()) {
			// File needs different options than current document so create new.
			SwitchDocumentAt(buffers.Current(), wEditor.CreateDocument(0, loadingOptions));
			allowUndoLoad = false;
		}

		if (allowUndoLoad) {
			wEditor.BeginUndoAction();
		} else {
			wEditor.SetUndoCollection(false);
		}

		OpenCurrentFile(fileSize, of & ofQuiet, asynchronous);

		if (allowUndoLoad) {
			wEditor.EndUndoAction();
		} else {
			wEditor.EmptyUndoBuffer();
		}

		CurrentBuffer()->isReadOnly = props.GetInt("read.only");
		wEditor.SetReadOnly(CurrentBuffer()->isReadOnly);
	}
	SetBuffersMenu();
	RemoveFileFromStack(filePath);
	DeleteFileStackMenu();
	SetFileStackMenu();
	SetWindowName();
	if (lineNumbers && lineNumbersExpand)
		SetLineNumberWidth();
	UpdateStatusBar(true);
	if (extender && !asynchronous)
		extender->OnOpen(filePath.AsUTF8().c_str());
	return true;
}

// Returns true if editor should get the focus
bool SciTEBase::OpenSelected() {
	std::string selName = SelectionFilename();
	if (selName.length() == 0) {
		WarnUser(warnWrongFile);
		return false;	// No selection
	}

#if !defined(GTK)
	if (selName.starts_with("http:") ||
			selName.starts_with("https:") ||
			selName.starts_with("ftp:") ||
			selName.starts_with("ftps:") ||
			selName.starts_with("news:") ||
			selName.starts_with("mailto:")) {
		std::string cmd = selName;
		AddCommand(cmd, "", JobSubsystem::shell);
		return false;	// Job is done
	}
#endif

	if (selName.starts_with("file://")) {
		selName.erase(0, 7);
		if (selName[0] == '/' && selName[2] == ':') { // file:///C:/filename.ext
			selName.erase(0, 1);
		}
	}

	if (selName.starts_with("~/")) {
		selName.erase(0, 2);
		const FilePath selPath(GUI::StringFromUTF8(selName));
		const FilePath expandedPath(FilePath::UserHomeDirectory(), selPath);
		selName = expandedPath.AsUTF8();
	}

	std::string fileNameForExtension = ExtensionFileName();
	std::string openSuffix = props.GetNewExpandString("open.suffix.", fileNameForExtension);
	selName += openSuffix;

	if (EqualCaseInsensitive(selName.c_str(), FileNameExt().AsUTF8().c_str()) || EqualCaseInsensitive(selName.c_str(), filePath.AsUTF8().c_str())) {
		WarnUser(warnWrongFile);
		return true;	// Do not open if it is the current file!
	}

	std::string cTag;
	SA::Line lineNumber = 0;
	if (IsPropertiesFile(filePath) &&
			(selName.find('.') == std::string::npos)) {
		// We are in a properties file and try to open a file without extension,
		// we suppose we want to open an imported .properties file
		// So we append the correct extension to open the included file.
		// Maybe we should check if the filename is preceded by "import"...
		selName += extensionProperties;
	} else {
		// Check if we have a line number (error message or grep result)
		// A bit of duplicate work with DecodeMessage, but we don't know
		// here the format of the line, so we do guess work.
		// Can't do much for space separated line numbers anyway...
		size_t endPath = selName.find('(');
		if (endPath != std::string::npos) {	// Visual Studio error message: F:\scite\src\SciTEBase.h(312):	bool Exists(
			lineNumber = atol(selName.c_str() + endPath + 1);
		} else {
			endPath = selName.find(':', 2);	// Skip Windows' drive separator
			if (endPath != std::string::npos) {	// grep -n line, perhaps gcc too: F:\scite\src\SciTEBase.h:312:	bool Exists(
				lineNumber = atol(selName.c_str() + endPath + 1);
			}
		}
		if (lineNumber > 0) {
			selName.erase(endPath);
		}

		// Support the ctags format

		if (lineNumber == 0) {
			cTag = GetCTag(pwFocussed);
		}
	}

	FilePath path;
	// Don't load the path of the current file if the selected
	// filename is an absolute pathname
	GUI::gui_string selFN = GUI::StringFromUTF8(selName);
	if (!FilePath(selFN).IsAbsolute()) {
		path = filePath.Directory();
		// If not there, look in openpath
		if (!Exists(path.AsInternal(), selFN.c_str(), nullptr)) {
			GUI::gui_string openPath = GUI::StringFromUTF8(props.GetNewExpandString(
							   "openpath.", fileNameForExtension));
			while (openPath.length()) {
				GUI::gui_string tryPath(openPath);
				const size_t sepIndex = tryPath.find(listSepString);
				if ((sepIndex != GUI::gui_string::npos) && (sepIndex != 0)) {
					tryPath.erase(sepIndex);
					openPath.erase(0, sepIndex + 1);
				} else {
					openPath.erase();
				}
				if (Exists(tryPath.c_str(), selFN.c_str(), nullptr)) {
					path.Set(tryPath.c_str());
					break;
				}
			}
		}
	}
	FilePath pathReturned;
	if (Exists(path.AsInternal(), selFN.c_str(), &pathReturned)) {
		// Open synchronously if want to seek line number or search tag
		const OpenFlags of = ((lineNumber > 0) || (cTag.length() != 0)) ? ofSynchronous : ofNone;
		if (Open(pathReturned, of)) {
			if (lineNumber > 0) {
				wEditor.GotoLine(lineNumber - 1);
			} else if (!cTag.empty()) {
				const SA::Line cTagLine = IntPtrFromString(cTag, 0);
				if (cTagLine > 0) {
					wEditor.GotoLine(cTagLine - 1);
				} else {
					findWhat = cTag;
					FindNext(false);
				}
			}
			return true;
		}
	} else {
		WarnUser(warnWrongFile);
	}
	return false;
}

namespace {

// Find the portions that are the same at the start and end of two string_views.
// When views equal return (length, 0).
std::pair<size_t, size_t> CommonEnds(std::string_view a, std::string_view b) noexcept {
	const size_t length = std::min<size_t>(a.length(), b.length());
	size_t start = 0;
	while ((start < length) && (a[start] == b[start])) {
		start++;
	}
	const size_t maxLeft = length - start;
	size_t last = 0;
	while ((last < maxLeft) && (a[a.length() - last - 1] == b[b.length() - last - 1])) {
		last++;
	}
	return { start, last };
}

}

void SciTEBase::Revert() {
	if (filePath.IsUntitled()) {
		wEditor.ClearAll();
	} else {
		const FilePosition fp = GetFilePosition();
		const long long fileLength = filePath.GetFileLength();
		const UniMode uniMode = CurrentBuffer()->unicodeMode;
		if ((fileLength < 1000000) && (uniMode == UniMode::cookie || uniMode == UniMode::uni8Bit || uniMode == UniMode::utf8)) {
			// If short and file and memory use same encoding
			const std::string contents = filePath.Read();
			// Check for BOM that matches file mode
			const std::string_view svUtf8BOM(UTF8BOM);
			if ((uniMode == UniMode::utf8) && !contents.starts_with(svUtf8BOM)) {
				// Should have BOM but doesn't so use full load
				OpenCurrentFile(fileLength, false, false);
			} else {
				std::string_view viewContents = contents;
				if (uniMode == UniMode::utf8) {
					// Has BOM but should be omitted in editor
					viewContents.remove_prefix(svUtf8BOM.length());
				}
				const std::string_view doc = TextAsView();
				const std::pair<size_t, size_t> ends = CommonEnds(doc, viewContents);
				const size_t start = ends.first;
				const size_t last = ends.second;
				if ((viewContents.length() != doc.length()) || (start != doc.length())) {
					// Truncate and insert
					wEditor.SetTarget(SA::Span(start, doc.length() - last));
					const std::string_view changed = viewContents.substr(start, viewContents.size() - last - start);
					wEditor.ReplaceTarget(changed);
				}
				wEditor.SetSavePoint();
			}
		} else {
			OpenCurrentFile(fileLength, false, false);
		}
		DisplayAround(fp);
	}
}

std::string_view SciTEBase::TextAsView() {
	const SA::Position length = wEditor.Length();
	const char *documentMemory = static_cast<const char *>(wEditor.CharacterPointer());
	return std::string_view(documentMemory, length);
}

void SciTEBase::CheckReload() {
	if (props.GetInt("load.on.activate")) {
		// Make a copy of fullPath as otherwise it gets aliased in Open
		const time_t newModTime = filePath.ModifiedTime();
		if ((newModTime != 0) && (newModTime != CurrentBuffer()->fileModTime)) {
			const FilePosition fp = GetFilePosition();
			const OpenFlags of = props.GetInt("reload.preserves.undo") ? ofPreserveUndo : ofNone;
			if (CurrentBuffer()->isDirty || props.GetInt("are.you.sure.on.reload") != 0) {
				if ((0 == dialogsOnScreen) && (newModTime != CurrentBuffer()->fileModLastAsk)) {
					GUI::gui_string msg;
					if (CurrentBuffer()->isDirty) {
						msg = LocaliseMessage(
							      "The file '^0' has been modified. Should it be reloaded?",
							      filePath.AsInternal());
					} else {
						msg = LocaliseMessage(
							      "The file '^0' has been modified outside SciTE. Should it be reloaded?",
							      FileNameExt().AsInternal());
					}
					const MessageBoxChoice decision = WindowMessageBox(wSciTE, msg, mbsYesNo | mbsIconQuestion);
					if (decision == MessageBoxChoice::yes) {
						Open(filePath, static_cast<OpenFlags>(of | ofForceLoad));
						DisplayAround(fp);
					}
					CurrentBuffer()->fileModLastAsk = newModTime;
				}
			} else {
				Open(filePath, static_cast<OpenFlags>(of | ofForceLoad));
				DisplayAround(fp);
			}
		}  else if (newModTime == 0 && CurrentBuffer()->fileModTime != 0)  {
			// Check if the file is deleted
			CurrentBuffer()->fileModTime = 0;
			CurrentBuffer()->fileModLastAsk = 0;
			CurrentBuffer()->isDirty = true;
			CheckMenus();
			SetWindowName();
			SetBuffersMenu();
			GUI::gui_string msg = LocaliseMessage(
						      "The file '^0' has been deleted.",
						      filePath.AsInternal());
			WindowMessageBox(wSciTE, msg, mbsOK);
		}
	}
}

void SciTEBase::Activate(bool activeApp) {
	if (activeApp) {
		CheckReload();
	} else {
		if (props.GetInt("save.on.deactivate")) {
			SaveTitledBuffers();
		}
	}
}

FilePath SciTEBase::SaveName(const char *ext) const {
	if (!ext) {
		return filePath;
	}
	const FilePath directory = filePath.Directory();
	GUI::gui_string name = filePath.Name().AsInternal();
	const size_t dot = name.rfind('.');
	if (dot != GUI::gui_string::npos) {
		const int keepExt = props.GetInt("export.keep.ext");
		if (keepExt == 0) {
			name.erase(dot);
		} else if (keepExt == 2) {
			name[dot] = '_';
		}
	}
	name += GUI::StringFromUTF8(ext);
	return FilePath(directory, name);
}

SciTEBase::SaveResult SciTEBase::SaveIfUnsure(bool forceQuestion, SaveFlags sf) {
	CurrentBuffer()->failedSave = false;
	if (CurrentBuffer()->pFileWorker) {
		if (CurrentBuffer()->pFileWorker->IsLoading())
			// In semi-loaded state so refuse to save
			return SaveResult::cancelled;
		else
			return SaveResult::completed;
	}
	if ((CurrentBuffer()->isDirty) && (LengthDocument() || !filePath.IsUntitled() || forceQuestion)) {
		if (props.GetInt("are.you.sure", 1) ||
				filePath.IsUntitled() ||
				forceQuestion) {
			GUI::gui_string msg;
			if (!filePath.IsUntitled()) {
				msg = LocaliseMessage("Save changes to '^0'?", filePath.AsInternal());
			} else {
				msg = LocaliseMessage("Save changes to (Untitled)?");
			}
			const MessageBoxChoice decision = WindowMessageBox(wSciTE, msg, mbsYesNoCancel | mbsIconQuestion);
			if (decision == MessageBoxChoice::yes) {
				if (!Save(sf))
					return SaveResult::cancelled;
			}
			return (decision == MessageBoxChoice::cancel) ? SaveResult::cancelled : SaveResult::completed;
		} else {
			if (!Save(sf))
				return SaveResult::cancelled;
		}
	}
	return SaveResult::completed;
}

SciTEBase::SaveResult SciTEBase::SaveIfUnsureAll() {
	if (SaveAllBuffers(false) == SaveResult::cancelled) {
		return SaveResult::cancelled;
	}
	if (props.GetInt("save.recent")) {
		for (int i = 0; i < buffers.lengthVisible; ++i) {
			const Buffer &buff = buffers.buffers[i];
			AddFileToStack(buff.file);
		}
	}
	if (props.GetInt("save.session") || props.GetInt("save.position") || props.GetInt("save.recent")) {
		SaveSessionFile(GUI_TEXT(""));
	}

	if (extender && extender->NeedsOnClose()) {
		// Ensure extender is told about each buffer closing
		for (BufferIndex k = 0; k < buffers.lengthVisible; k++) {
			SetDocumentAt(k);
			extender->OnClose(filePath.AsUTF8().c_str());
		}
	}

	// Any buffers that have been read but not marked read should be marked
	// read and their loaders deleted
	for (Buffer &buffer : buffers.buffers) {
		if (buffer.lifeState == Buffer::LifeState::readAll) {
			buffer.CompleteLoading();
		}
	}

	// Definitely going to exit now, so delete all documents
	// Set editor back to initial document
	if (buffers.lengthVisible > 0) {
		wEditor.SetDocPointer(buffers.buffers[0].doc.get());
	}
	// Release all the extra documents
	for (Buffer &buffer : buffers.buffers) {
		if (buffer.doc && !buffer.pFileWorker) {
			buffer.doc.reset();
		}
	}
	// Initial document will be deleted when editor deleted
	return SaveResult::completed;
}

SciTEBase::SaveResult SciTEBase::SaveIfUnsureForBuilt() {
	if (props.GetInt("save.all.for.build")) {
		return SaveAllBuffers(!props.GetInt("are.you.sure.for.build"));
	}
	if (CurrentBuffer()->isDirty) {
		if (props.GetInt("are.you.sure.for.build"))
			return SaveIfUnsure(true);

		Save();
	}
	return SaveResult::completed;
}

/**
	Selection saver and restorer.

	If virtual space is disabled, the class does nothing.

	If virtual space is enabled, constructor saves all selections using (line, column) coordinates,
	destructor restores all the saved selections.
**/
class SelectionKeeper {
public:
	explicit SelectionKeeper(GUI::ScintillaWindow &editor) : wEditor(editor) {
		const SA::VirtualSpace mask = static_cast<SA::VirtualSpace>(
						      static_cast<int>(SA::VirtualSpace::RectangularSelection) |
						      static_cast<int>(SA::VirtualSpace::UserAccessible));
		if (static_cast<int>(wEditor.VirtualSpaceOptions()) & static_cast<int>(mask)) {
			const int n = wEditor.Selections();
			for (int i = 0; i < n; ++i) {
				selections.push_back(LocFromPos(GetSelection(i)));
			}
		}
	}

	// Deleted so SelectionKeeper objects can not be copied.
	SelectionKeeper(const SelectionKeeper &) = delete;
	SelectionKeeper(SelectionKeeper &&) = delete;
	SelectionKeeper &operator=(const SelectionKeeper &) = delete;
	SelectionKeeper &operator=(SelectionKeeper &&) = delete;

	~SelectionKeeper() {
		try {
			// Should never throw unless there was an earlier failure in Scintilla.
			// This is just for restoring selection so swallow exceptions.
			int i = 0;
			for (auto const &sel : selections) {
				SetSelection(i, PosFromLoc(sel));
				++i;
			}
		} catch (...) {
			// Ignore exceptions
		}
	}

private:
	struct Position {
		Position(SA::Position pos_, SA::Position virt_) noexcept : pos(pos_), virt(virt_) {};
		SA::Position pos;
		SA::Position virt;
	};

	struct Location {
		Location(SA::Line line_, SA::Position col_) noexcept : line(line_), col(col_) {};
		SA::Line line;
		SA::Position col;
	};

	Position GetAnchor(int i) {
		const SA::Position pos  = wEditor.SelectionNAnchor(i);
		const SA::Position virt = wEditor.SelectionNAnchorVirtualSpace(i);
		return Position(pos, virt);
	}

	Position GetCaret(int i) {
		const SA::Position pos  = wEditor.SelectionNCaret(i);
		const SA::Position virt = wEditor.SelectionNCaretVirtualSpace(i);
		return Position(pos, virt);
	}

	std::pair<Position, Position> GetSelection(int i) {
		return {GetAnchor(i), GetCaret(i)};
	};

	Location LocFromPos(Position const &pos) {
		const SA::Line line = wEditor.LineFromPosition(pos.pos);
		const SA::Position col  = wEditor.Column(pos.pos) + pos.virt;
		return Location(line, col);
	}

	std::pair<Location, Location> LocFromPos(std::pair<Position, Position> const &pos) {
		return {LocFromPos(pos.first), LocFromPos(pos.second)};
	}

	Position PosFromLoc(Location const &loc) {
		const SA::Position pos = wEditor.FindColumn(loc.line, loc.col);
		const SA::Position col = wEditor.Column(pos);
		return Position(pos, loc.col - col);
	}

	std::pair<Position, Position> PosFromLoc(std::pair<Location, Location> const &loc) {
		return {PosFromLoc(loc.first), PosFromLoc(loc.second)};
	}

	void SetAnchor(int i, Position const &pos) {
		wEditor.SetSelectionNAnchor(i, pos.pos);
		wEditor.SetSelectionNAnchorVirtualSpace(i, pos.virt);
	};

	void SetCaret(int i, Position const &pos) {
		wEditor.SetSelectionNCaret(i, pos.pos);
		wEditor.SetSelectionNCaretVirtualSpace(i, pos.virt);
	}

	void SetSelection(int i, std::pair<Position, Position> const &pos) {
		SetAnchor(i, pos.first);
		SetCaret(i, pos.second);
	}

	GUI::ScintillaWindow &wEditor;
	std::vector<std::pair<Location, Location>> selections;
};

void SciTEBase::StripTrailingSpaces() {
	const SA::Line maxLines = wEditor.LineCount();
	SelectionKeeper keeper(wEditor);
	for (SA::Line line = 0; line < maxLines; line++) {
		const SA::Position lineStart = wEditor.LineStart(line);
		const SA::Position lineEnd = wEditor.LineEnd(line);
		SA::Position firstSpace = lineEnd;
		while ((firstSpace > lineStart) && IsSpaceOrTab(wEditor.CharacterAt(firstSpace-1))) {
			firstSpace--;
		}
		if (firstSpace < lineEnd) {
			wEditor.DeleteRange(firstSpace, lineEnd-firstSpace);
		}
	}
}

void SciTEBase::EnsureFinalNewLine() {
	const SA::Line maxLines = wEditor.LineCount();
	bool appendNewLine = maxLines == 1;
	const SA::Position endDocument = wEditor.LineStart(maxLines);
	if (maxLines > 1) {
		appendNewLine = endDocument > wEditor.LineStart(maxLines - 1);
	}
	if (appendNewLine) {
		const char *eol = LineEndString(wEditor.EOLMode());
		wEditor.InsertText(endDocument, eol);
	}
}

// Perform any changes needed before saving such as normalizing spaces and line ends.
bool SciTEBase::PrepareBufferForSave(const FilePath &saveName) {
	bool retVal = false;
	// Perform clean ups on text before saving
	UndoBlock ub(wEditor);
	if (stripTrailingSpaces)
		StripTrailingSpaces();
	if (ensureFinalLineEnd)
		EnsureFinalNewLine();
	if (ensureConsistentLineEnds)
		wEditor.ConvertEOLs(wEditor.EOLMode());

	if (extender)
		retVal = extender->OnBeforeSave(saveName.AsUTF8().c_str());

	return retVal;
}

/**
 * Writes the buffer to the given filename.
 */
bool SciTEBase::SaveBuffer(const FilePath &saveName, SaveFlags sf) {
	bool retVal = PrepareBufferForSave(saveName);

	if (!retVal) {

		FILE *fp = saveName.Open(fileWrite);
		if (fp) {
			const size_t lengthDoc = LengthDocument();
			if (!(sf & sfSynchronous)) {
				wEditor.SetReadOnly(true);
				const std::string_view documentBytes = TextAsView();
				CurrentBuffer()->pFileWorker = std::make_unique<FileStorer>(this, documentBytes, saveName, fp, CurrentBuffer()->unicodeMode, (sf & sfProgressVisible));
				CurrentBuffer()->pFileWorker->sleepTime = props.GetInt("asynchronous.sleep");
				if (PerformOnNewThread(CurrentBuffer()->pFileWorker.get())) {
					retVal = true;
				} else {
					GUI::gui_string msg = LocaliseMessage("Failed to save file '^0' as thread could not be started.", saveName.AsInternal());
					WindowMessageBox(wSciTE, msg);
				}
			} else {
				std::unique_ptr<Utf8_16::Writer> convert = Utf8_16::Writer::Allocate(CurrentBuffer()->unicodeMode, blockSize);
				std::vector<char> data(blockSize);
				retVal = true;
				for (size_t startBlock = 0; startBlock < lengthDoc;) {
					size_t grabSize = std::min(lengthDoc - startBlock, blockSize);
					// Round down so only whole characters retrieved.
					grabSize = wEditor.PositionBefore(startBlock + grabSize + 1) - startBlock;
					const SA::Span rangeGrab(startBlock, startBlock + grabSize);
					CopyText(wEditor, data.data(), rangeGrab);
					const size_t written = convert->fwrite(std::string_view(data.data(), grabSize), fp);
					if (written == 0) {
						retVal = false;
						break;
					}
					startBlock += grabSize;
				}
				if (fclose(fp) != 0) {
					retVal = false;
				}
				fp = nullptr;
			}
		}
	}

	if (retVal && extender && (sf & sfSynchronous)) {
		extender->OnSave(saveName.AsUTF8().c_str());
	}
	UpdateStatusBar(true);
	return retVal;
}

void SciTEBase::ReloadProperties() {
	ReadGlobalPropFile();
	SetImportMenu();
	ReadLocalPropFile();
	ReadAbbrevPropFile();
	ReadProperties();
	SetWindowName();
	BuffersMenu();
	Redraw();
}

// Returns false if cancelled or failed to save
bool SciTEBase::Save(SaveFlags sf) {
	if (!filePath.IsUntitled()) {
		GUI::gui_string msg;
		if (CurrentBuffer()->ShouldNotSave()) {
			msg = LocaliseMessage(
				      "The file '^0' has not yet been loaded entirely, so it can not be saved right now. Please retry in a while.",
				      filePath.AsInternal());
			WindowMessageBox(wSciTE, msg);
			// It is OK to not save this file
			return true;
		}

		if (CurrentBuffer()->pFileWorker) {
			msg = LocaliseMessage(
				      "The file '^0' is already being saved.",
				      filePath.AsInternal());
			WindowMessageBox(wSciTE, msg);
			// It is OK to not save this file
			return true;
		}

		if (props.GetInt("save.deletes.first")) {
			filePath.Remove();
		} else if (props.GetInt("save.check.modified.time")) {
			const time_t newModTime = filePath.ModifiedTime();
			if ((newModTime != 0) && (CurrentBuffer()->fileModTime != 0) &&
					(newModTime != CurrentBuffer()->fileModTime)) {
				msg = LocaliseMessage("The file '^0' has been modified outside SciTE. Should it be saved?",
						      filePath.AsInternal());
				const MessageBoxChoice decision = WindowMessageBox(wSciTE, msg, mbsYesNo | mbsIconQuestion);
				if (decision == MessageBoxChoice::no) {
					return false;
				}
			}
		}

		if ((LengthDocument() <= props.GetInt("background.save.size", -1)) ||
				(buffers.SingleBuffer()))
			sf = static_cast<SaveFlags>(sf | sfSynchronous);
		if (SaveBuffer(filePath, sf)) {
			CurrentBuffer()->SetTimeFromFile();
			if (sf & sfSynchronous) {
				wEditor.SetSavePoint();
				if (IsPropertiesFile(filePath)) {
					ReloadProperties();
				}
			}
		} else {
			if (!CurrentBuffer()->failedSave) {
				CurrentBuffer()->failedSave = true;
				msg = LocaliseMessage(
					      "Could not save file '^0'. Save under a different name?", filePath.AsInternal());
				const MessageBoxChoice decision = WindowMessageBox(wSciTE, msg, mbsYesNo | mbsIconWarning);
				if (decision == MessageBoxChoice::yes) {
					return SaveAsDialog();
				}
			}
			return false;
		}
		return true;
	} else {
		if (props.GetString("save.path.suggestion").length()) {
			const time_t t = time(nullptr);
			char timeBuff[15];
			strftime(timeBuff, sizeof(timeBuff), "%Y%m%d%H%M%S",  localtime(&t));
			PropSetFile propsSuggestion;
			propsSuggestion.superPS = &props;  // Allow access to other settings
			propsSuggestion.Set("TimeStamp", timeBuff);
			propsSuggestion.SetPath("SciteUserHome", GetSciteUserHome());
			std::string savePathSuggestion = propsSuggestion.GetExpandedString("save.path.suggestion");
			std::ranges::replace(savePathSuggestion, '\\', '/');  // To accept "\" on Unix
			if (savePathSuggestion.size() > 0) {
				filePath = FilePath(GUI::StringFromUTF8(savePathSuggestion)).NormalizePath();
			}
		}
		const bool ret = SaveAsDialog();
		if (!ret)
			filePath.Set(GUI_TEXT(""));
		return ret;
	}
}

void SciTEBase::SaveAs(const GUI::gui_char *file, bool fixCase) {
	SetFileName(file, fixCase);
	Save();
	ReadProperties();
	wEditor.ClearDocumentStyle();
	wEditor.Colourise(0, wEditor.LineStart(1));
	Redraw();
	SetWindowName();
	BuffersMenu();
	if (extender)
		extender->OnSave(filePath.AsUTF8().c_str());
}

bool SciTEBase::SaveIfNotOpen(const FilePath &destFile, bool fixCase) {
	FilePath absPath = destFile.AbsolutePath();
	const BufferIndex index = buffers.GetDocumentByName(absPath, true /* excludeCurrent */);
	if (index >= 0) {
		GUI::gui_string msg = LocaliseMessage(
					      "File '^0' is already open in another buffer.", destFile.AsInternal());
		WindowMessageBox(wSciTE, msg);
		return false;
	} else {
		SaveAs(absPath.AsInternal(), fixCase);
		return true;
	}
}

void SciTEBase::AbandonAutomaticSave() {
	CurrentBuffer()->AbandonAutomaticSave();
}

bool SciTEBase::IsStdinBlocked() noexcept {
	return false; /* always default to blocked */
}

void SciTEBase::OpenFromStdin(bool UseOutputPane) {
	std::unique_ptr<Utf8_16::Reader> convert = Utf8_16::Reader::Allocate();
	std::vector<char> data(blockSize);

	/* if stdin is blocked, do not execute this method */
	if (IsStdinBlocked())
		return;

	Open(FilePath());
	GUI::ScintillaWindow &wText = UseOutputPane ? wOutput : wEditor;
	if (!UseOutputPane) {
		wEditor.BeginUndoAction();	// Group together clear and insert
	}
	wText.ClearAll();
	size_t lenFile = fread(data.data(), 1, data.size(), stdin);
	while (lenFile > 0) {
		const std::string_view dataConverted = convert->convert(std::string_view(data.data(), lenFile));
		AddText(wText, dataConverted);
		lenFile = fread(data.data(), 1, data.size(), stdin);
	}
	if (UseOutputPane) {
		if (props.GetInt("split.vertical") == 0) {
			heightOutput = 2000;
		} else {
			heightOutput = 500;
		}
		SizeSubWindows();
	} else {
		wEditor.EndUndoAction();
	}
	CurrentBuffer()->unicodeMode = convert->getEncoding();
	if (CurrentBuffer()->unicodeMode != UniMode::uni8Bit) {
		// Override the code page if Unicode
		codePage = SA::CpUtf8;
	} else {
		codePage = props.GetInt("code.page");
	}
	if (UseOutputPane) {
		wOutput.SetSel(0, 0);
	} else {
		wEditor.SetCodePage(codePage);

		// Zero all the style bytes
		wEditor.ClearDocumentStyle();

		CurrentBuffer()->overrideExtension = "x.txt";
		ReadProperties();
		SetIndentSettings();
		wEditor.ColouriseAll();
		Redraw();

		wEditor.SetSel(0, 0);
	}
}

void SciTEBase::OpenFilesFromStdin() {
	char data[8 * 1024] {};

	/* if stdin is blocked, do not execute this method */
	if (IsStdinBlocked())
		return;

	while (fgets(data, sizeof(data) - 1, stdin)) {
		char *pNL;
		if ((pNL = strchr(data, '\n')) != nullptr)
			* pNL = '\0';
		Open(GUI::StringFromUTF8(data), ofQuiet);
	}
	if (buffers.lengthVisible == 0)
		Open(FilePath());
}

class BufferedFile {
	FileHolder fp;
	bool readAll;
	bool exhausted;
	enum {bufLen = 64 * 1024};
	char buffer[bufLen];
	size_t pos;
	size_t valid;
	void EnsureData() noexcept {
		if (pos >= valid) {
			if (readAll || !fp) {
				exhausted = true;
			} else {
				valid = fread(buffer, 1, bufLen, fp.get());
				if (valid < bufLen) {
					readAll = true;
				}
				pos = 0;
			}
		}
	}
public:
	explicit BufferedFile(const FilePath &fPath) : fp(fPath.Open(fileRead)) {
		readAll = false;
		exhausted = !fp;
		buffer[0] = 0;
		pos = 0;
		valid = 0;
	}
	bool Exhausted() const noexcept {
		return exhausted;
	}
	char NextByte() noexcept {
		EnsureData();
		if (pos >= valid) {
			return 0;
		}
		return buffer[pos++];
	}
	bool BufferContainsNull() noexcept {
		EnsureData();
		for (size_t i = 0; i < valid; i++) {
			if (buffer[i] == '\0')
				return true;
		}
		return false;
	}
};

class FileReader {
	std::unique_ptr<BufferedFile> bf;
	int lineNum;
	bool lastWasCR;
	std::string lineToCompare;
	std::string lineToShow;
	bool caseSensitive;
public:
	FileReader(const FilePath &fPath, bool caseSensitive_) : bf(std::make_unique<BufferedFile>(fPath)) {
		lineNum = 0;
		lastWasCR = false;
		caseSensitive = caseSensitive_;
	}
	// Deleted so FileReader objects can not be copied.
	FileReader(const FileReader &) = delete;
	const char *Next() {
		if (bf->Exhausted()) {
			return nullptr;
		}
		lineToShow.clear();
		while (!bf->Exhausted()) {
			const char ch = bf->NextByte();
			if (lastWasCR && ch == '\n' && lineToShow.empty()) {
				lastWasCR = false;
			} else if (ch == '\r' || ch == '\n') {
				lastWasCR = ch == '\r';
				break;
			} else {
				lineToShow.push_back(ch);
			}
		}
		lineNum++;
		lineToCompare = lineToShow;
		if (!caseSensitive) {
			LowerCaseAZ(lineToCompare);
		}
		return lineToCompare.c_str();
	}
	int LineNumber() const noexcept {
		return lineNum;
	}
	const char *Original() const noexcept {
		return lineToShow.c_str();
	}
	bool BufferContainsNull() noexcept {
		return bf->BufferContainsNull();
	}
};

constexpr bool IsWordCharacter(int ch) noexcept {
	return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z')  || (ch >= '0' && ch <= '9')  || (ch == '_');
}

bool SciTEBase::GrepIntoDirectory(const FilePath &directory) {
	const GUI::gui_char *sDirectory = directory.AsInternal();
	return sDirectory[0] != '.';
}

void SciTEBase::GrepRecursive(GrepFlags gf, const FilePath &baseDir, const char *searchString,
	GUI::gui_string_view fileTypes, GUI::gui_string_view excludedTypes) {
	constexpr int checkAfterLines = 10'000;
	FilePathSet directories;
	FilePathSet files;
	baseDir.List(directories, files);
	const size_t searchLength = strlen(searchString);
	std::string os;
	for (const FilePath &fPath : files) {
		if (jobQueue.Cancelled())
			return;
		if ((fileTypes.empty() || fPath.Matches(fileTypes)) &&
			((excludedTypes.empty() || !fPath.Matches(excludedTypes)))) {
			//OutputAppendStringSynchronised(fPath.AsUTF8());
			//OutputAppendStringSynchronised("\n");
			FileReader fr(fPath, FlagIsSet(gf, GrepFlags::matchCase));
			if (FlagIsSet(gf, GrepFlags::binary) || !fr.BufferContainsNull()) {
				while (const char *line = fr.Next()) {
					if (((fr.LineNumber() % checkAfterLines) == 0) && jobQueue.Cancelled())
						return;
					const char *match = strstr(line, searchString);
					if (match) {
						if (FlagIsSet(gf, GrepFlags::wholeWord)) {
							const char *lineEnd = line + strlen(line);
							while (match) {
								if (((match == line) || !IsWordCharacter(match[-1])) &&
										((match + searchLength == (lineEnd)) || !IsWordCharacter(match[searchLength]))) {
									break;
								}
								match = strstr(match + 1, searchString);
							}
						}
						if (match) {
							os.append(fPath.AsUTF8());
							os.append(":");
							std::string lNumber = StdStringFromInteger(fr.LineNumber());
							os.append(lNumber);
							os.append(":");
							os.append(fr.Original());
							os.append("\n");
						}
					}
				}
			}
		}
	}
	if (os.length()) {
		if (FlagIsSet(gf, GrepFlags::stdOut)) {
			fwrite(os.c_str(), os.length(), 1, stdout);
		} else {
			OutputAppendStringSynchronised(os);
		}
	}
	for (const FilePath &fPath : directories) {
		if (FlagIsSet(gf, GrepFlags::dot) || GrepIntoDirectory(fPath.Name())) {
			if ((excludedTypes.empty() || !fPath.Matches(excludedTypes))) {
				GrepRecursive(gf, fPath, searchString, fileTypes, excludedTypes);
			}
		}
	}
}

void SciTEBase::InternalGrep(GrepFlags gf, const FilePath &directory, GUI::gui_string_view fileTypes, GUI::gui_string_view excludedTypes,
	std::string_view search, SA::Position &originalEnd) {
	GUI::ElapsedTime commandTime;
	if (!FlagIsSet(gf, GrepFlags::stdOut)) {
		std::string os;
		os.append(">Internal search for \"");
		os.append(search);
		os.append("\" in \"");
		os.append(GUI::UTF8FromString(fileTypes));
		os.append("\"\n");
		OutputAppendStringSynchronised(os);
		ShowOutputOnMainThread();
		originalEnd += os.length();
	}
	std::string searchString(search);
	if (!FlagIsSet(gf, GrepFlags::matchCase)) {
		LowerCaseAZ(searchString);
	}
	GrepRecursive(gf, directory, searchString.c_str(), fileTypes, excludedTypes);
	if (!FlagIsSet(gf, GrepFlags::stdOut)) {
		std::string sExitMessage(">");
		if (jobQueue.TimeCommands()) {
			sExitMessage += "    Time: ";
			sExitMessage += StdStringFromDouble(commandTime.Duration(), 3);
		}
		sExitMessage += "\n";
		OutputAppendStringSynchronised(sExitMessage);
	}
}

