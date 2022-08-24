/*
-txtcmp <fil1> <file2> -outfile "C:\report.txt"
checks each line of a text file, reports lines that do not match

-onlyunk
option for txtcmp, will only report on lines containing the word unknown

-monitor -folder <path>
Check if changes occur to PR1 files
if PR1 is edited, check for existing related csv file
if no related csv, create new one and add the changes
else appened to the related csv

-sumfile <file>
option for monitor, changes will be concatenated to a specified file

-saveto <folder>
saves
/**/

#include <cstdio>
#include <fstream>
#include <string>
#include <iostream>
#include <sstream>
#include <windows.h>
#include <vector>
#include <algorithm>
#include <iterator>
#include <time.h>
#include "resource.h"
#include <cstring>
#include "sha256.h"

// to get current dir

#include <direct.h>
#define GetCurrentDir _getcwd

/// simple file watcher starts here
#include <FileWatcher/FileWatcherWin32.h>
#include <FileWatcher/FileWatcher.h>
#include <FileWatcher/FileWatcherImpl.h>

#define _WIN32_WINNT 0x0550
#define FILEWATCHER_IMPL FileWatcherWin32

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "ole32.lib")
#pragma warning (disable: 4996)

namespace FW
{
	/// Internal watch data
	struct WatchStruct
	{
		OVERLAPPED mOverlapped;
		HANDLE mDirHandle;
		BYTE mBuffer[32 * 1024];
		LPARAM lParam;
		DWORD mNotifyFilter;
		bool mStopNow;
		FileWatcherImpl* mFileWatcher;
		FileWatchListener* mFileWatchListener;
		char* mDirName;
		WatchID mWatchid;
		bool mIsRecursive;
	};

#pragma region Internal Functions

	// forward decl
	bool RefreshWatch(WatchStruct* pWatch, bool _clear = false);

	/// Unpacks events and passes them to a user defined callback.
	void CALLBACK WatchCallback(DWORD dwErrorCode, DWORD dwNumberOfBytesTransfered, LPOVERLAPPED lpOverlapped)
	{
		TCHAR szFile[MAX_PATH];
		PFILE_NOTIFY_INFORMATION pNotify;
		WatchStruct* pWatch = (WatchStruct*) lpOverlapped;
		size_t offset = 0;

		if(dwNumberOfBytesTransfered == 0)
			return;

		if (dwErrorCode == ERROR_SUCCESS)
		{
			do
			{
				pNotify = (PFILE_NOTIFY_INFORMATION) &pWatch->mBuffer[offset];
				offset += pNotify->NextEntryOffset;

#			if defined(UNICODE)
				{
					lstrcpynW(szFile, pNotify->FileName,
						min(MAX_PATH, pNotify->FileNameLength / sizeof(WCHAR) + 1));
				}
#			else
				{
					int count = WideCharToMultiByte(CP_ACP, 0, pNotify->FileName,
						pNotify->FileNameLength / sizeof(WCHAR),
						szFile, MAX_PATH - 1, NULL, NULL);
					szFile[count] = TEXT('\0');
				}
#			endif

				pWatch->mFileWatcher->handleAction(pWatch, szFile, pNotify->Action);

			} while (pNotify->NextEntryOffset != 0);
		}

		if (!pWatch->mStopNow)
		{
			RefreshWatch(pWatch);
		}
	}

	/// Refreshes the directory monitoring.
	bool RefreshWatch(WatchStruct* pWatch, bool _clear)
	{
		return ReadDirectoryChangesW(
			pWatch->mDirHandle, pWatch->mBuffer, sizeof(pWatch->mBuffer), pWatch->mIsRecursive,
			pWatch->mNotifyFilter, NULL, &pWatch->mOverlapped, _clear ? 0 : WatchCallback) != 0;
	}

	/// Stops monitoring a directory.
	void DestroyWatch(WatchStruct* pWatch)
	{
		if (pWatch)
		{
			pWatch->mStopNow = TRUE;

			CancelIo(pWatch->mDirHandle);

			RefreshWatch(pWatch, true);

			if (!HasOverlappedIoCompleted(&pWatch->mOverlapped))
			{
				SleepEx(5, TRUE);
			}

			CloseHandle(pWatch->mOverlapped.hEvent);
			CloseHandle(pWatch->mDirHandle);
			delete pWatch->mDirName;
			HeapFree(GetProcessHeap(), 0, pWatch);
		}
	}

	/// Starts monitoring a directory.
	WatchStruct* CreateWatch(LPCTSTR szDirectory, bool recursive, DWORD mNotifyFilter)
	{
		WatchStruct* pWatch;
		size_t ptrsize = sizeof(*pWatch);
		pWatch = static_cast<WatchStruct*>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, ptrsize));

		pWatch->mDirHandle = CreateFile(szDirectory, FILE_LIST_DIRECTORY,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
			OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, NULL);

		if (pWatch->mDirHandle != INVALID_HANDLE_VALUE)
		{
			pWatch->mOverlapped.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
			pWatch->mNotifyFilter = mNotifyFilter;
			pWatch->mIsRecursive = recursive;

			if (RefreshWatch(pWatch))
			{
				return pWatch;
			}
			else
			{
				CloseHandle(pWatch->mOverlapped.hEvent);
				CloseHandle(pWatch->mDirHandle);
			}
		}

		HeapFree(GetProcessHeap(), 0, pWatch);
		return NULL;
	}

#pragma endregion

	//--------
	FileWatcherWin32::FileWatcherWin32()
		: mLastWatchID(0)
	{
	}

	//--------
	FileWatcherWin32::~FileWatcherWin32()
	{
		WatchMap::iterator iter = mWatches.begin();
		WatchMap::iterator end = mWatches.end();
		for(; iter != end; ++iter)
		{
			DestroyWatch(iter->second);
		}
		mWatches.clear();
	}

	//--------
	WatchID FileWatcherWin32::addWatch(const String& directory, FileWatchListener* watcher, bool recursive)
	{
		WatchID watchid = ++mLastWatchID;

		WatchStruct* watch = CreateWatch(directory.c_str(), recursive,
			FILE_NOTIFY_CHANGE_CREATION | FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_FILE_NAME);

		if(!watch)
			throw FileNotFoundException(directory);

		watch->mWatchid = watchid;
		watch->mFileWatcher = this;
		watch->mFileWatchListener = watcher;
		watch->mDirName = new char[directory.length()+1];
		strcpy(watch->mDirName, directory.c_str());

		mWatches.insert(std::make_pair(watchid, watch));

		return watchid;
	}

	//--------
	void FileWatcherWin32::removeWatch(const String& directory)
	{
		WatchMap::iterator iter = mWatches.begin();
		WatchMap::iterator end = mWatches.end();
		for(; iter != end; ++iter)
		{
			if(directory == iter->second->mDirName)
			{
				removeWatch(iter->first);
				return;
			}
		}
	}

	//--------
	void FileWatcherWin32::removeWatch(WatchID watchid)
	{
		WatchMap::iterator iter = mWatches.find(watchid);

		if(iter == mWatches.end())
			return;

		WatchStruct* watch = iter->second;
		mWatches.erase(iter);

		DestroyWatch(watch);
	}

	//--------
	void FileWatcherWin32::update()
	{
		MsgWaitForMultipleObjectsEx(0, NULL, 0, QS_ALLINPUT, MWMO_ALERTABLE);
	}

	//--------
	void FileWatcherWin32::handleAction(WatchStruct* watch, const String& filename, unsigned long action)
	{
		Action fwAction;

		switch(action)
		{
		case FILE_ACTION_RENAMED_NEW_NAME:
		case FILE_ACTION_ADDED:
			fwAction = Actions::Add;
			break;
		case FILE_ACTION_RENAMED_OLD_NAME:
		case FILE_ACTION_REMOVED:
			fwAction = Actions::Delete;
			break;
		case FILE_ACTION_MODIFIED:
			fwAction = Actions::Modified;
			break;
		};

		watch->mFileWatchListener->handleFileAction(watch->mWatchid, watch->mDirName, filename, fwAction);
	}

};//namespace FW
namespace FW
{

	//--------
	FileWatcher::FileWatcher()
	{
		mImpl = new FILEWATCHER_IMPL();
	}

	//--------
	FileWatcher::~FileWatcher()
	{
		delete mImpl;
		mImpl = 0;
	}

	//--------
	WatchID FileWatcher::addWatch(const String& directory, FileWatchListener* watcher)
	{
		return mImpl->addWatch(directory, watcher, false);
	}

	//--------
	WatchID FileWatcher::addWatch(const String& directory, FileWatchListener* watcher, bool recursive)
	{
		return mImpl->addWatch(directory, watcher, recursive);
	}

	//--------
	void FileWatcher::removeWatch(const String& directory)
	{
		mImpl->removeWatch(directory);
	}

	//--------
	void FileWatcher::removeWatch(WatchID watchid)
	{
		mImpl->removeWatch(watchid);
	}

	//--------
	void FileWatcher::update()
	{
		mImpl->update();
	}

};//namespace FW

/// simple file watcher ends here


using namespace std;


int lstCnt = 0;
typedef vector<string> stringvec;

struct csvdiff {
	string TIMESTAMP;
	string MODEL;
	string FIELD;
	string FROM;
	string TO;
	};
struct field_item {
	uint16_t id = 0;
	uint32_t addr = 0;
	uint16_t len = 0;
	};
struct lst_data {
	int id = 0;
	string name = "PLACE HOLDER FOR NAME";
	string datatype = "PLACEHOLDER FOR DATATPYE";
	string units = "PLACE HOLDER FOR UNITS";
	bool isSkipped = false;
	};
fletcher32(const uint16_t *data, size_t len) {
	// https://en.wikipedia.org/w/index.php?title=Fletcher%27s_checksum
	uint32_t c0, c1;
	unsigned int i;

	for (c0 = c1 = 0; len >= 360; len -= 360) {
		for (i = 0; i < 360; ++i) {
			c0 = c0 + *data++;
			c1 = c1 + c0;
			}
		c0 = c0 % 65535;
		c1 = c1 % 65535;
		}
	for (i = 0; i < len; ++i) {
		c0 = c0 + *data++;
		c1 = c1 + c0;
		}
	c0 = c0 % 65535;
	c1 = c1 % 65535;
	return (c1 << 16 | c0);
	}
string get_date(void) {
	time_t now;
	int MAX_DATE = 16;
	char the_date[MAX_DATE];
	the_date[0] = '\0';
	now = time(NULL);
	if (now != -1) {
		strftime(the_date, MAX_DATE, "_%Y%m%d%H%M%S", gmtime(&now));
		}
	return string(the_date);
	}
string get_part_date(const string &datepart) {
    time_t     now = time(0);
    struct tm  tstruct;
    char       buf[80];
    tstruct = *localtime(&now);
    // Visit http://en.cppreference.com/w/cpp/chrono/c/strftime
    // for more information about date/time format
    strftime(buf, sizeof(buf), datepart.c_str(), &tstruct);

    return buf;
	}
//string rtrim(string str) {
//	int start2 = 0, end2 = str.length() - 1;
//	while(isspace(str[start2]) || isspace(str[end2]))
//	{
//		if(isspace(str[start2]))
//			start2++;
//		if(isspace(str[end2]))
//			end2--;
//	}
//	str.erase(0,start2);
//	str.erase((end2 - start2) + 1);
//	return str;
//	}
string rtrim(string str) {
	if (str.size() > 0) {
		string blank(str.size(), ' ');
		if (str!=blank) {
			int start2 = 0, end2 = str.length() - 1;
			while(isspace(str[start2]) || isspace(str[end2])) {
				if(isspace(str[start2]))
					start2++;
				if(isspace(str[end2]))
					end2--;
				}
			str.erase(0,start2);
			str.erase((end2 - start2) + 1);
			}
		else {
			str = "UNKNOWN";
			}
		}
	return str;
	}
string trim(const string &s) {
    string::const_iterator it = s.begin();
    while (it != s.end() && isspace(*it))
        it++;

    string::const_reverse_iterator rit = s.rbegin();
    while (rit.base() != it && isspace(*rit))
        rit++;

    return string(it, rit.base());
	}
void replaceExt(string &s, const string &newExt) {
	string::size_type i = s.rfind('.', s.length());
	if (i != string::npos) {
		s.replace(i+1, newExt.length(), newExt);
		}
	}
string ReplaceString(std::string subject, const std::string& search, const std::string& replace) {
    // Czarek Tomczak
    // https://stackoverflow.com/questions/3418231/replace-part-of-a-string-with-another-string/3418285
    size_t pos = 0;
    while ((pos = subject.find(search, pos)) != std::string::npos) {
         subject.replace(pos, search.length(), replace);
         pos += replace.length();
    	}
    return subject;
	}
string getFilenameType(string const &path ) {
	string ext;
	// Find the last dot, if any.
	size_t dotIdx = path.find_last_of( "." );
	if ( dotIdx != string::npos ) {
		// Find the last directory separator, if any.
		size_t dirSepIdx = path.find_last_of( "/\\" );
		// If the dot is at the beginning of the file name, do not treat it as a file extension.
		// e.g., a hidden file:  ".alpha".
		// This test also incidentally avoids a dot that is really a current directory indicator.
		// e.g.:  "alpha/./bravo"
		if ( dotIdx > dirSepIdx + 1 ) {
			ext = path.substr( dotIdx );
			}
		}
	return ext;
	}
string toupper(const string &s) {
    // getFilenameType file   -- returns: ".jpg"
    std::string ret(s.size(), char());
    for(unsigned int i = 0; i < s.size(); ++i)
        ret[i] = (s[i] <= 'z' && s[i] >= 'a') ? s[i]-('a'-'A') : s[i];
    return ret;
	}
string IntToString (double a) {
	string result = "";
    ostringstream temp;
    temp << a;
	if (temp.good ()) {
		result = temp.str();
		}
    return result;
	}
string IntToHexString(int number, int length) {
    string s;
    ostringstream temp;
    temp << std::hex << number;
    s = toupper(temp.str());
    s.insert(s.begin(), length - s.length(), '0');
    return ("0x" + s);
    }
bool is_file_exist(string fileName) {
    const char *test;
    test = fileName.c_str();
    ifstream infile(test);
    return infile.good();
    }
string getFilenamePathRoot(const string &str) {
    size_t found;
    string strt;
    found = str.find_last_of("/\\");
    if (found < str.size()) {
        strt = str.substr(0, found);
        found = strt.find_last_of("/\\");
        return strt.substr(found + 1, -1);
        }
    else {
        return str;
        }
    }
string getFilenamePath (const string &str) {
    // getFilenamePath file   -- returns: "g:\subdir1\subdir2\"
    size_t found;
    string strt;
    found = str.find_last_of("/\\");
    if (found < str.size()) {
        strt = str.substr(0, found);
        return (strt + "\\");
        }
    else {
        return "";
        }
    }
string filenameFromPath(const string &str) {
    // filenameFromPath file  -- returns: "myImage.jpg"
    size_t found;
    string strt;
    found = str.find_last_of("/\\");
    if (found < str.size()) {
        strt = str.substr(found + 1, -1);
        return strt;
        }
    else {
        return str;
        }
    }
string getFilenameFile(const string &str) {
    // getFilenameFile file   -- returns: "myImage"
    size_t found;
    string strt;
    found = str.find_last_of("/\\");
    if (found < str.size()) {
        strt = str.substr(found + 1, -1);
        found = strt.find(".");
        if (found < strt.size()) {
            strt = strt.substr(0, found);
            }
        //return strt;
        }
    else {
		strt = str;
        //return str;
        }
    size_t lastdot = strt.find_last_of(".");
    if (lastdot == std::string::npos) return strt;
    return strt.substr(0, lastdot);
	}
string padString (string str, int len, string pad, bool addToLeft = true) {
    string temp = str;
    string padding = "";
    if (temp.size() > len) {
        temp = temp.substr(0, len);
        }
    if ((len - temp.size()) > 0) {
        for (int i = 0; i < ((len - temp.size())); i++){
            padding += pad;
            }
        }
	if (addToLeft) {
		return (padding + temp);
		}
    else {
		return (temp + padding);
    	}
    }
string GetCurrentWorkingDir( void ) {
    char buff[FILENAME_MAX];
    GetCurrentDir( buff, FILENAME_MAX );
    string current_working_dir(buff);
    return current_working_dir;
    }
string getCurrentPath (const char* argvStr) {
	// had some compatiblity problem, so using alternative function
	string inFolder = GetCurrentWorkingDir();
	//inFolder = getFilenamePath((string)argvStr);

	//char appFilePath[255] = "";
	//_fullpath(appFilePath, argvStr, sizeof(appFilePath));
	//inFolder = getFilenamePath(appFilePath);

	if (inFolder.substr(inFolder.length() - 1, 1) != "\\") {
		inFolder += "\\";
		}
	return inFolder;
	}
string paddstring(int number, int length, char chr, bool addToLeft = true) {
    string s;
    ostringstream temp;
    if (addToLeft) {
        temp << std::hex << number;
        s = temp.str();
        s.insert(s.begin(), length - s.length(), chr);
        }
    else {
        temp << number;
        s = temp.str();
        s.append(length - s.length(), chr);
        }
    return ("0x" + s);
    }
void read_directory(string path, const string &fext, stringvec &v) {


    string pattern(path + fext);
    //pattern.append("\\*");
    //cout << "pattern " << pattern << endl;
    WIN32_FIND_DATA data;
    HANDLE hFind;
    if ((hFind = FindFirstFile(pattern.c_str(), &data)) != INVALID_HANDLE_VALUE) {
        //cout << "file " << (path + fext) << endl;
        do {
            //v.push_back(path + data.cFileName);
            v.insert( v.end(), (path + data.cFileName) );
        	} while (FindNextFile(hFind, &data) != 0);
        FindClose(hFind);
    	}
	}
void split (string str, string delimiter, int &numFound, string* &result) {
    // seperate string into each time delimiter character is found
	size_t pos = 0;
	int num = 1;
	string s = str;
	//string* result;
	// pre-pass get number of seperations
	while ((pos = s.find(delimiter)) != string::npos) {
		s.erase(0, pos + delimiter.length());
		num++;
		}
	numFound = num;
	// if pre-pass, then create array
	if (numFound > 0) {
	    num = 0;
	    s = str;
	    //delete[] result;
		result = new string[numFound];
		while ((pos = s.find(delimiter)) != string::npos) {
			result[num] = s.substr(0, pos);
			s.erase(0, pos + delimiter.length());
			num++;
			}
		result[num] = s.substr(0, pos);
		}
	// return array
	//return result;
	}
string getSplitString (string input, char delimiter, int index) {
    int counter = 0;
    size_t pos = 0;
    string s = input;
    string result = "";
    for (int i = 0; i < s.size(); i++) {
        if (s[i] == delimiter) counter++;
        }
    if (index <= counter) {
        counter = 0;
        while ((pos = s.find(delimiter)) <= string::npos) {
        	result = s.substr(0, pos);
        	s.erase(0, pos + 1);
        	if (index == counter) {
        	    break;
        	    }
        	counter++;
        	}
        }
    return result;
    }
void readFieldsFromPR1File (string** &pr1files, stringvec &files, lst_data* &lstfile) {
	// reads file, return an array if collected values
    double float64;
    string str(256, ' ');
    string* ss;
    int sc = 0;
    int index = 0;
    field_item fi;
    uint32_t fsize = 0;
    uint32_t cur = 0;

	pr1files = new string*[files.size()];

    for (int x = 0; x < files.size(); x++) {
        pr1files[x] = new string[lstCnt];
        ifstream file (files[x].c_str(), ios::in|ios::binary|ios::ate);
        if (file.is_open()) {
            cur = 0;
            fsize = file.tellg();
            while ((cur + 1)< fsize) {
                file.seekg (cur, ios::beg);
                file.read (reinterpret_cast<char *>(&fi.id), sizeof(fi.id));
                file.seekg (cur + 2, ios::beg);
                file.read (reinterpret_cast<char *>(&fi.len), sizeof(fi.len));
                fi.addr = cur + 4;
                file.seekg (fi.addr, ios::beg);
                cur = fi.addr + fi.len;
                if (lstCnt > 0) {
                    for (int i = 0; i < lstCnt; i++){
                        if (fi.id == lstfile[i].id) {
                            if (lstfile[i].isSkipped == false) {
                                if ((toupper(lstfile[i].datatype)).find("DOUBLE",0) <= lstfile[i].datatype.size()) {
                                    file.read (reinterpret_cast<char *>(&float64), sizeof(float64));
                                    pr1files[x][i] = IntToString(float64);
                                    index = lstfile[i].datatype.find("[",0);
                                    if (index <= lstfile[i].datatype.size()) {
                                        split(lstfile[i].datatype.substr(index + 1,(lstfile[i].datatype.find("]",0)) - (index + 1)), ",", sc, ss);
                                        if (float64 <= sc) {
                                            pr1files[x][i] = ss[(int)float64];
                                            }
                                        delete[] ss;
                                        sc = 0;
                                        }
                                    }
                                else if ((toupper(lstfile[i].datatype)).find("STRING",0) <= lstfile[i].datatype.size()) {
                                    str.clear();
                                    str.resize(fi.len, ' ');
                                    file.read(&str[0], fi.len);
                                    pr1files[x][i] = str.c_str();
                                    }
                                }
                            }
                        }
                    }
                }
            file.close();
            } else cout << "Unable to open file\n";
        }
	}
void printLST (string savepath, bool showNotePad) {
	ofstream lstfile;
	lstfile.open(savepath.c_str(),ios::out);
	lstfile <<
		"# ID list\n" <<
		"#\n" <<
		"# '#' at start of line act as comment\n" <<
		"# and line will be ignored\n" <<
		"#\n" <<
		"# table struct as such <<\n" <<
		"#\n" <<
		"# <ID> <Name> <datatype[parm]> <units>\n" <<
		"#\n" <<
		"# !Tabs must seperate data\n" <<
		"# \n" <<
		"# id may be supplied as a decimal, or\n" <<
		"# as a hex.\n" <<
		"# if hex number must contain x prior\n" <<
		"# to the number. ex 0x01 or x01 or hx01\n" <<
		"# \n" <<
		"# negative numbers will be ignored,\n" <<
		"# ex -1 will mean to print the column\n" <<
		"# in the csv as a place holder.\n" <<
		"# you only need id and name\n" <<
		"#\n" <<
		"# name is what will be the heading \n" <<
		"# saved to the csv\n" <<
		"# \n" <<
		"# datatype is important for reading\n" <<
		"# the data correctly\n" <<
		"# correctly used types include:\n" <<
		"# \n" <<
		"# Double\n" <<
		"#  64bit float\n" <<
		"# String\n" <<
		"#  array of characters to form text\n" <<
		"#\n" <<
		"# I know there are more, but I was too\n" <<
		"# lazy to add them in.\n" <<
		"# I also figure they must have byte\n" <<
		"# arrays to store images, but I \n" <<
		"# did not investigate\n" <<
		"#\n" <<
		"# asides from the binary file's datatype\n" <<
		"# the meaning of the number can be\n" <<
		"# something different.\n" <<
		"# so after datatype in brackets define\n" <<
		"# any enumerators.\n" <<
		"# \n" <<
		"# ex Double[On,Off]\n" <<
		"# if the double returns 0, then it will\n" <<
		"# display as On. if the number is 1,\n" <<
		"# then it will return as Off.\n" <<
		"# \n" <<
		"# units are optional, but if you would\n" <<
		"# like to define the units used you can\n" <<
		"# ex. mm, mm/s, kg, Prts, secs, etc\n" <<
		"# not really sure what to do with it...\n" <<
		"# \n" <<
		"# thats pretty much all i can think of,\n" <<
		"# goodluck\n" <<
		"# -Corey\n" <<
		"# \n" <<
		"# \n" <<
		"#0x0001	version	String\n" <<
		"#0x02BB	format	String\n" <<
		"-1	Case Sensitive Barcodes\n" <<
		"#PRODUCT NAME\n" <<
		"0x0459	GERBER FILENAME	String\n" <<
		"0x2710	PRODUCT ID	String\n" <<
		"0x045B	Soft Rail Land	Double[False,True]\n" <<
		"0x045C	Soft Rail Lift	Double[False,True]\n" <<
		"#0x045D	soft_rail_land_speed	Double\n" <<
		"#0x045E	soft_rail_land_distance	Double\n" <<
		"#0x045F	soft_rail_lift_speed	Double\n" <<
		"#0x0460	soft_rail_lift_distance	Double\n" <<
		"0x0002	PRODUCT BARCODE	String\n" <<
		"0x274C	DWELL SPEED	Double	mm\n" <<
		"0x274D	DWELL HEIGHT	Double	mm/s\n" <<
		"0x0004	SCREEN ADAPTOR	Double[NONE,1255,Sanyo,Heraeus,20x20,12x12]\n" <<
		"0x0005	SCREEN IMAGE	Double[EDGE,CENTRE]\n" <<
		"0x0078	CUSTOM SCREEN	Double[DISABLED,ENABLED]\n" <<
		"0x000A	BOARD WIDTH	Double	mm\n" <<
		"0x000B	BOARD LENGTH	Double	mm\n" <<
		"0x000C	BOARD THICKNESS	Double	mm\n" <<
		"-1	SCREEN THICKNESS\n" <<
		"#0x03E9	screen_x_forward	double\n" <<
		"#0x03EA	screen_x_rear	double\n" <<
		"#0x03EB	screen_y	double\n" <<
		"#0x0069	print_area_length	double\n" <<
		"#0x0064	print_area_width	double\n" <<
		"0x01C8	FRONT PRINT SPEED	Double	mm/s\n" <<
		"0x01C7	REAR PRINT SPEED	Double	mm/s\n" <<
		"0x0049	PRINT FRONT LIMIT	Double	mm\n" <<
		"0x004A	PRINT REAR LIMIT	Double	mm\n" <<
		"0x0010	FRONT PRESSURE	Double	kg\n" <<
		"0x052E	PASTE PRESSURE DELAY	Double	secs\n" <<
		"0x0011	REAR PRESSURE	Double	kg\n" <<
		"0x000F	FLOOD HEIGHT	Double	mm\n" <<
		"#0x01C4	flood_speed	Double\n" <<
		"0x0024	KNEAD BOARDS	Double\n" <<
		"0x0022	PASTE KNEAD PERIOD	Double	minutes\n" <<
		"0x050C	PRINT GAP	Double	mm\n" <<
		"-1	SEPARATION DELAY\n" <<
		"0x0013	SEPARATION SPEED	Double	mm/s\n" <<
		"0x2715	SEPARATION DISTANCE	Double	mm\n" <<
		"0x0048	UNDER CLEARANCE	Double	mm\n" <<
		"-1	EXTENDED PRINT GAP\n" <<
		"0x0026	BOARD COUNT	Double	boards\n" <<
		"-1	STINGER HARDWARE\n" <<
		"0x0014	PRINT MODE	Double[Print/Print,Print/Flood,Flood/Print,Adhesive]\n" <<
		"-1	PASTE RIDGE REMOVAL\n" <<
		"-1	STINGER CAL LOC X\n" <<
		"-1	STINGER CAL LOC Y\n" <<
		"-1	Stinger Laser Offset Y\n" <<
		"-1	Stinger Laser Offset X\n" <<
		"-1	Stinger COM Port\n" <<
		"0x0015	PRINT DEPOSITS	Double	Prts\n" <<
		"0x001E	PASTE DISPENSE RATE	Double	Prts\n" <<
		"0x001F	PASTE DISPENSE SPEED	Double	mm/s\n" <<
		"-1	PASTE WHILE CLEAN\n" <<
		"-1	PASTE WITH BOARD\n" <<
		"-1	ALTERNATE DISP\n" <<
		"-1	ALTERNATE DISP RATE\n" <<
		"0x0113	PASTE RECOVERY RATE	Double\n" <<
		"0x0111	FRONT PASTE RECOVERY	Double	mm\n" <<
		"0x0112	REAR PASTE RECOVERY	Double	mm\n" <<
		"-1	PASTE RECOVERY SQUEEGEE PRESSURE\n" <<
		"-1	BLUE CLEAN MODE 1\n" <<
		"-1	Prime Paste Dispenses\n" <<
		"0x0028	CLEAN RATE 1	Double\n" <<
		"-1	BLUE CLEAN MODE 2\n" <<
		"0x002A	CLEAN RATE 2	Double\n" <<
		"-1	BLUE CLEAN AFTER KNEAD\n" <<
		"-1	BLUE CLEAN AFTER DOWNTIME\n" <<
		"-1	CLEAN AFTER TIME\n" <<
		"-1	BLUE DRY CLEAN SPEED\n" <<
		"-1	BLUE WET CLEAN SPEED\n" <<
		"-1	BLUE VAC CLEAN SPEED\n" <<
		"-1	VAC CLEAN START TIME\n" <<
		"0x75A5	VACUUM ON PERIOD	Double\n" <<
		"-1	FRONT START OFFSET\n" <<
		"-1	REAR START OFFSET\n" <<
		"0x0023	KNEAD DEPOSITS	Double\n" <<
		"-1	KNEAD AFTER DISPENSE\n" <<
		"-1	FRONT KNEAD SPEED\n" <<
		"-1	REAR KNEAD SPEED\n" <<
		"0x01C5	REAR KNEAD PRESSURE	Double\n" <<
		"0x01C6	FRONT KNEAD PRESSURE	Double\n" <<
		"-1	SELECTIVE PRINT\n" <<
		"0x2AF8	BOARD 1 FID. TYPE	Double[Circle,Rectangle,Diamond,Triangle,Double Square,Cross,Video Model]\n" <<
		"#0x2AF9	fiducial_1_board_width	Double\n" <<
		"#0x2AFA	fiducial_1_board_height	Double\n" <<
		"#0x2AFB	fiducial_1_board_outer_contour	Double\n" <<
		"#0x2AFC	fiducial_1_board_inner_contour	Double\n" <<
		"#0x2AFD	fiducial_1_board_rounding	Double\n" <<
		"#0x2AFE	fiducial_1_board_rotation	Double\n" <<
		"#0x2B00	fiducial_1_board_score	Double\n" <<
		"0x2BC0	BOARD 2 FID. TYPE	Double[Circle,Rectangle,Diamond,Triangle,Double Square,Cross,Video Model]\n" <<
		"#0x2BC1	fiducial_2_board_width	Double\n" <<
		"#0x2BC2	fiducial_2_board_height	Double\n" <<
		"#0x2BC3	fiducial_2_board_outer_contour	Double\n" <<
		"#0x2BC4	fiducial_2_board_inner_contour	Double\n" <<
		"#0x2BC5	fiducial_2_board_rounding	Double\n" <<
		"#0x2BC6	fiducial_2_board_rotation	Double\n" <<
		"#0x2BC8	fiducial_2_board_score	Double\n" <<
		"0x2C88	BOARD 3 FID. TYPE	Double[Circle,Rectangle,Diamond,Triangle,Double Square,Cross,Video Model]\n" <<
		"#0x2C89	fiducial_3_board_width	Double\n" <<
		"#0x2C8A	fiducial_3_board_height	Double\n" <<
		"#0x2C8B	fiducial_3_board_outer_contour	Double\n" <<
		"#0x2C8C	fiducial_3_board_inner_contour	Double\n" <<
		"#0x2C8D	fiducial_3_board_rounding	Double\n" <<
		"#0x2C8E	fiducial_3_board_rotation	Double\n" <<
		"#0x2C90	fiducial_3_board_score	Double\n" <<
		"0x2B5C	SCREEN 1 FID. TYPE	Double[Circle,Rectangle,Diamond,Triangle,Double Square,Cross,Video Model]\n" <<
		"#0x2B5D	fiducial_1_screen_width	Double\n" <<
		"#0x2B5E	fiducial_1_screen_height	Double\n" <<
		"#0x2B5F	fiducial_1_screen_outer_contour	Double\n" <<
		"#0x2B60	fiducial_1_screen_inner_contour	Double\n" <<
		"#0x2B61	fiducial_1_screen_rounding	Double\n" <<
		"#0x2B62	fiducial_1_screen_rotation	Double\n" <<
		"#0x2B64	fiducial_1_screen_score	Double\n" <<
		"0x2C24	SCREEN 2 FID. TYPE	Double[Circle,Rectangle,Diamond,Triangle,Double Square,Cross,Video Model]\n" <<
		"#0x2C25	fiducial_2_screen_width	Double\n" <<
		"#0x2C26	fiducial_2_screen_height	Double\n" <<
		"#0x2C27	fiducial_2_screen_outer_contour	Double\n" <<
		"#0x2C28	fiducial_2_screen_inner_contour	Double\n" <<
		"#0x2C29	fiducial_2_screen_rounding	Double\n" <<
		"#0x2C2A	fiducial_2_screen_rotation	Double\n" <<
		"#0x2C2C	fiducial_2_screen_score	Double\n" <<
		"0x2CEC	SCREEN 3 FID. TYPE	Double[Circle,Rectangle,Diamond,Triangle,Double Square,Cross,Video Model]\n" <<
		"#0x2CED	fiducial_3_screen_width	Double\n" <<
		"#0x2CEE	fiducial_3_screen_height	Double\n" <<
		"#0x2CEF	fiducial_3_screen_outer_contour	Double\n" <<
		"#0x2CF0	fiducial_3_screen_inner_contour	Double\n" <<
		"#0x2CF1	fiducial_3_screen_rounding	Double\n" <<
		"#0x2CF2	fiducial_3_screen_rotation	Double\n" <<
		"#0x2CF4	fiducial_3_screen_score	Double\n" <<
		"0x0033	FIDUCIAL 1 X	Double\n" <<
		"0x0034	FIDUCIAL 1 Y	Double\n" <<
		"0x0035	FIDUCIAL 2 X	Double\n" <<
		"0x0036	FIDUCIAL 2 Y	Double\n" <<
		"0x0037	FIDUCIAL 3 X	Double\n" <<
		"0x0038	FIDUCIAL 3 Y	Double\n" <<
		"0x0039	FORWARD X OFFSET	Double\n" <<
		"0x003A	FORWARD Y OFFSET	Double\n" <<
		"0x003B	FORWARD W OFFSETt	Double\n" <<
		"0x003C	REVERSE X OFFSET	Double\n" <<
		"0x003D	REVERSE Y OFFSET	Double\n" <<
		"0x003E	REVERSE W OFFSET	Double\n" <<
		"-1	VIEW ROUGH ALIGNMENT\n" <<
		"0x0042	ALIGNMENT WEIGHTING	Double\n" <<
		"0x0043	X ALIGN WEIGHTING	Double\n" <<
		"0x0044	Y ALIGN WEIGHTING	Double\n" <<
		"0x0032	ALIGNMENT MODE	Double[2 FIDUCIAL,3 FIDUCIAL]\n" <<
		"-1	TOOLING TYPE\n" <<
		"-1	Double Align Mode\n" <<
		"-1	BOARD-AT-STOP RUN-ON\n" <<
		"0x0046	BOARD STOP X	Double\n" <<
		"0x0047	BOARD STOP Y	Double\n" <<
		"0x0020	PASTE START	Double\n" <<
		"0x0021	PASTE STOP	Double\n" <<
		"#0x04BC	snugging_pressure	double\n" <<
		"#0x04BD	clamp_pressure	double\n" <<
		"#0x04BE	ots_retain_blade	double\n" <<
		"-1	SPC CONFIGURATION\n" <<
		"-1	PRESS DECAY INTERVAL\n" <<
		"-1	Snugging Force\n" <<
		"-1	BStop Retract Delay\n" <<
		"#0x36B7	mesh_front	double\n" <<
		"#0x36B8	mesh_rear	double\n" <<
		"#0x36B9	mesh_left	double\n" <<
		"#0x36BA	mesh_right	double\n" <<
		"#0x006E	dist_to_image	double\n";
	lstfile.close();
	if (showNotePad) {
		system (("start notepad.exe " + savepath).c_str());
		}
	}
void printCSV (string** data, lst_data* lstfile, int numItems, string savepath, bool showNotePad, stringvec filenames) {
	if (numItems > 0) { // num of files
        ofstream csvfile;
        csvfile.open(savepath.c_str(),ios::out);
        if (csvfile.is_open()) {
			// Headers
			csvfile << "FileName";
			for (int i = 0; i < lstCnt; i++){
				csvfile << "," << lstfile[i].name;
				}
			csvfile << endl;
			for (int i = 0; i < numItems; i++){
				csvfile << getFilenameFile(filenames[i]);
				for (int ii = 0; ii < lstCnt; ii++){
					if (lstfile[ii].isSkipped) {
						csvfile << ",";
						}
					else {
						csvfile << "," << data[i][ii];
						}
					}
				csvfile << endl;
				}
			}
		csvfile.close();
		if (showNotePad) {
			system (("start notepad.exe " + savepath).c_str());
			}
		}
	}
void printRAW (string path1, string savepath, lst_data* lstfile, bool showNotePad) {
	uint32_t num = 0;
	uint32_t numKnown = 0;
	uint32_t index = 0;
	uint32_t cur = 0;
	uint32_t fsize = 0;
	uint8_t byte8 = 0;
	uint16_t byte16 = 0;
	uint32_t byte32 = 0;
	double float64 = 0;
	string str(256, ' ');
	string label = "unidentified";
	string type = "unknown";
	string enums = "";
	string datasize = "0000";
	string data = "???????????";
	string suffix = "        ";
	field_item fi;
	ifstream file (path1.c_str(), ios::in|ios::binary|ios::ate);
	if (file.is_open()) {
		fsize = file.tellg();

		ofstream rawfile;
		rawfile.open(savepath.c_str(),ios::out);
		if (file.is_open()) {
			rawfile <<
				"filename: " <<
				savepath <<
				"\n\n" <<
				(padString("ID", 8, " ", false)) <<
				(padString("DATATYPE", 12, " ", false)) <<
				(padString("DATASIZE", 12, " ", false)) <<
				(padString("NAME", 32, " ", false)) <<
				(padString("DATA", 32, " ", false)) << endl;

			while (cur < fsize) {
				label = "";
				data = "";
				type = "";
				enums = "";
				datasize = "";
				suffix = "";
				file.seekg (cur, ios::beg);
				file.read (reinterpret_cast<char *>(&fi.id), sizeof(fi.id));
				file.seekg (cur + 2, ios::beg);
				file.read (reinterpret_cast<char *>(&fi.len), sizeof(fi.len));
				datasize = IntToString((double)fi.len);
				fi.addr = cur + 4;
				file.seekg (fi.addr, ios::beg);
				// Search if ID is already in ID Table
				// get name and datatype
				if (lstCnt > 0) {
					for (int i = 0; i < lstCnt; i++) {
						// loop through array, look for ID match
						if (fi.id == lstfile[i].id) {
							// id match found, assign varaibles
							label = lstfile[i].name;
							suffix = lstfile[i].units;
							type = lstfile[i].datatype;
							index = type.find("[",0);
							if (index <= type.size()) {
								//split(type.substr(index + 1,(type.find("]",0)) - (index + 1)), ",", sc, ss);
								enums = type.substr(index + 1,(type.find("]",0)) - (index + 1));
								type = type.substr(0, index);
								}
							type = toupper(type);
							// end loop
							numKnown++;
							break;
							}
						}
					}
				// datatype unknown, so try to guess it based on length
				if (type=="") {
					switch (fi.len) {
						case 1:
							type = "BYTE";
							break;
						case 2:
							type = "SHORT";
							break;
						case 4:
							type = "LONG";
							break;
						case 8:
							type = "DOUBLE";
							break;
						//case 20:
						//	type = "STRING";
						//	break;
						default:
							type = "ARRAY";
						}
					}
				// attempt to read data...
				if (type == "BYTE") {
					file.read (reinterpret_cast<char *>(&byte8), sizeof(byte8));
					data = IntToString((double)byte8);
					}
				if (type == "SHORT") {
					file.read (reinterpret_cast<char *>(&byte16), sizeof(byte16));
					data = IntToString((double)byte16);
					}
				if (type == "LONG") {
					file.read (reinterpret_cast<char *>(&byte32), sizeof(byte32));
					data = IntToString((double)byte32);
					}

				if (type == "DOUBLE") {
					file.read (reinterpret_cast<char *>(&float64), sizeof(float64));
					if (enums != "") {
						data = getSplitString(enums,',',(int)float64);
						}
					else {
						data = IntToString((double)float64);
						}
					}
				if (type == "STRING") {
					str.clear();
					str.resize(fi.len, ' ');
					file.read(&str[0], fi.len);
					data = "\"" + str + "\"";
					//data = str.c_str();
					}
				if (type == "ARRAY") {
					data = "";
					if (fi.len > 0) {
						str = "";
						for (int ii = 0; ii < fi.len; ii++) {
							file.seekg (fi.addr + ii, ios::beg);
							file.read (reinterpret_cast<char *>(&byte8), sizeof(byte8));
							str += IntToHexString((int)byte8, 2);
							str += " ";
							}
						if (str.size() > 0) {
							data = str.substr(0, str.size() - 1);
							}
						}
					}

				rawfile <<
					padString(IntToHexString(fi.id, 4), 8, " ", false) <<
					padString(type, 12, " ", false) <<
					padString(datasize, 12, " ", false) <<
					padString(label, 32, " ", false) <<
					data << endl;

				type.clear();
				label.clear();
				data.clear();
				datasize.clear();

				cur = fi.addr + fi.len;
				num++;
				}
			rawfile << "\nNumber of Entries: " << num << " (" << numKnown << "/" << num << ") Named" << endl;
			}
		if (showNotePad) {
			system (("start notepad.exe " + savepath).c_str());
			}
		rawfile.close();
		}
	file.close();
	}
signed long stringToInt (string s) {
    stringstream str(s);
	signed long num = 0;
	int i = 0;
    i = s.find("x",0);
    if (i <= s.size()) {
    	str << hex << "00000000" << s.substr(i + 1,s.size()-(i+1));
    	str >> num;
    	}
 	else {
		str >> num;
 		}
    return num;
	}
lst_data* readLST (string filename, lst_data* lstfile) {
	string str;
	int i;
	ifstream file (filename.c_str());
	if (file.is_open()) {
		int cnt = 0;
		while (getline(file, str)) {
			if (str.size() > 0) {
				if (str.substr(0,1) != "#") {
					cnt++;
					}
				}
			}
		lstCnt = cnt;
		lstfile = new lst_data[cnt];
		cnt = 0;
		file.clear();
		file.seekg(0, ios::beg);
		while (getline(file, str)) {
			if (str.size() > 0) {
				if (str.substr(0,1) != "#") {
					lstfile[cnt].id = -1;
					lstfile[cnt].datatype = "Double";
					lstfile[cnt].name = "";
					lstfile[cnt].units = "";
					str.substr(0,1);
					for (int ii=0; ii<4; ii++) {
						i = str.find("\t",0);
						if (i > str.size()) {
							i = str.size();
							}
						if (i <= str.size()) {
							switch (ii) {
								case 0:
									lstfile[cnt].id = (int)stringToInt(str.substr(0,i));
									break;
								case 1:
									lstfile[cnt].name = str.substr(0,i);
									break;
								case 2:
									lstfile[cnt].datatype = str.substr(0,i);

									break;
								case 3:
									lstfile[cnt].units = str.substr(0,i);
									break;
								}
							if (i + 1 < str.size()) {
								str = str.substr(i+1, str.size()-(i+1));
								}
							else {
								break;
								}
							}
						}
					cnt++;
					}
				}
			}
		}
	return lstfile;
	delete[] lstfile;
	}
void genLST (lst_data* &lstfile) {
	lstCnt = 107;
	lstfile = new lst_data[lstCnt];
	int i = 0;
	//lstfile[i].isSkipped=false;	lstfile[i].id=0x0001;	lstfile[i].name="version";	lstfile[i].datatype="String";	lstfile[i].units="";	i++;
	//lstfile[i].isSkipped=false;	lstfile[i].id=0x02BB;	lstfile[i].name="format";	lstfile[i].datatype="String";	lstfile[i].units="";	i++;
	lstfile[i].isSkipped=true;	lstfile[i].id=0xFFFF;	lstfile[i].name="Case Sensitive Barcodes";	lstfile[i].datatype="";	lstfile[i].units="";	i++;
	//lstfile[i].isSkipped=false;	lstfile[i].id=PRODUCT NAME;	lstfile[i].name="";	lstfile[i].datatype="";	lstfile[i].units="";	i++;
	lstfile[i].isSkipped=false;	lstfile[i].id=0x0459;	lstfile[i].name="GERBER FILENAME";	lstfile[i].datatype="String";	lstfile[i].units="";	i++;
	lstfile[i].isSkipped=false;	lstfile[i].id=0x2710;	lstfile[i].name="PRODUCT ID";	lstfile[i].datatype="String";	lstfile[i].units="";	i++;
	lstfile[i].isSkipped=false;	lstfile[i].id=0x045B;	lstfile[i].name="Soft Rail Land";	lstfile[i].datatype="Double[False,True]";	lstfile[i].units="";	i++;
	lstfile[i].isSkipped=false;	lstfile[i].id=0x045C;	lstfile[i].name="Soft Rail Lift";	lstfile[i].datatype="Double[False,True]";	lstfile[i].units="";	i++;
	//lstfile[i].isSkipped=false;	lstfile[i].id=0x045D;	lstfile[i].name="soft_rail_land_speed";	lstfile[i].datatype="Double";	lstfile[i].units="";	i++;
	//lstfile[i].isSkipped=false;	lstfile[i].id=0x045E;	lstfile[i].name="soft_rail_land_distance";	lstfile[i].datatype="Double";	lstfile[i].units="";	i++;
	//lstfile[i].isSkipped=false;	lstfile[i].id=0x045F;	lstfile[i].name="soft_rail_lift_speed";	lstfile[i].datatype="Double";	lstfile[i].units="";	i++;
	//lstfile[i].isSkipped=false;	lstfile[i].id=0x0460;	lstfile[i].name="soft_rail_lift_distance";	lstfile[i].datatype="Double";	lstfile[i].units="";	i++;
	lstfile[i].isSkipped=false;	lstfile[i].id=0x0002;	lstfile[i].name="PRODUCT BARCODE";	lstfile[i].datatype="String";	lstfile[i].units="";	i++;
	lstfile[i].isSkipped=false;	lstfile[i].id=0x274C;	lstfile[i].name="DWELL SPEED";	lstfile[i].datatype="Double";	lstfile[i].units="mm";	i++;
	lstfile[i].isSkipped=false;	lstfile[i].id=0x274D;	lstfile[i].name="DWELL HEIGHT";	lstfile[i].datatype="Double";	lstfile[i].units="mm/s";	i++;
	lstfile[i].isSkipped=false;	lstfile[i].id=0x0004;	lstfile[i].name="SCREEN ADAPTOR";	lstfile[i].datatype="Double[NONE,1255,Sanyo,Heraeus,20x20,12x12]";	lstfile[i].units="";	i++;
	lstfile[i].isSkipped=false;	lstfile[i].id=0x0005;	lstfile[i].name="SCREEN IMAGE";	lstfile[i].datatype="Double[EDGE,CENTRE]";	lstfile[i].units="";	i++;
	lstfile[i].isSkipped=false;	lstfile[i].id=0x0078;	lstfile[i].name="CUSTOM SCREEN";	lstfile[i].datatype="Double[DISABLED,ENABLED]";	lstfile[i].units="";	i++;
	lstfile[i].isSkipped=false;	lstfile[i].id=0x000A;	lstfile[i].name="BOARD WIDTH";	lstfile[i].datatype="Double";	lstfile[i].units="mm";	i++;
	lstfile[i].isSkipped=false;	lstfile[i].id=0x000B;	lstfile[i].name="BOARD LENGTH";	lstfile[i].datatype="Double";	lstfile[i].units="mm";	i++;
	lstfile[i].isSkipped=false;	lstfile[i].id=0x000C;	lstfile[i].name="BOARD THICKNESS";	lstfile[i].datatype="Double";	lstfile[i].units="mm";	i++;
	lstfile[i].isSkipped=true;	lstfile[i].id=0xFFFF;	lstfile[i].name="SCREEN THICKNESS";	lstfile[i].datatype="";	lstfile[i].units="";	i++;
	//lstfile[i].isSkipped=false;	lstfile[i].id=0x03E9;	lstfile[i].name="screen_x_forward";	lstfile[i].datatype="double";	lstfile[i].units="";	i++;
	//lstfile[i].isSkipped=false;	lstfile[i].id=0x03EA;	lstfile[i].name="screen_x_rear";	lstfile[i].datatype="double";	lstfile[i].units="";	i++;
	//lstfile[i].isSkipped=false;	lstfile[i].id=0x03EB;	lstfile[i].name="screen_y";	lstfile[i].datatype="double";	lstfile[i].units="";	i++;
	//lstfile[i].isSkipped=false;	lstfile[i].id=0x0069;	lstfile[i].name="print_area_length";	lstfile[i].datatype="double";	lstfile[i].units="";	i++;
	//lstfile[i].isSkipped=false;	lstfile[i].id=0x0064;	lstfile[i].name="print_area_width";	lstfile[i].datatype="double";	lstfile[i].units="";	i++;
	lstfile[i].isSkipped=false;	lstfile[i].id=0x01C8;	lstfile[i].name="FRONT PRINT SPEED";	lstfile[i].datatype="Double";	lstfile[i].units="mm/s";	i++;
	lstfile[i].isSkipped=false;	lstfile[i].id=0x01C7;	lstfile[i].name="REAR PRINT SPEED";	lstfile[i].datatype="Double";	lstfile[i].units="mm/s";	i++;
	lstfile[i].isSkipped=false;	lstfile[i].id=0x0049;	lstfile[i].name="PRINT FRONT LIMIT";	lstfile[i].datatype="Double";	lstfile[i].units="mm";	i++;
	lstfile[i].isSkipped=false;	lstfile[i].id=0x004A;	lstfile[i].name="PRINT REAR LIMIT";	lstfile[i].datatype="Double";	lstfile[i].units="mm";	i++;
	lstfile[i].isSkipped=false;	lstfile[i].id=0x0010;	lstfile[i].name="FRONT PRESSURE";	lstfile[i].datatype="Double";	lstfile[i].units="kg";	i++;
	lstfile[i].isSkipped=false;	lstfile[i].id=0x052E;	lstfile[i].name="PASTE PRESSURE DELAY";	lstfile[i].datatype="Double";	lstfile[i].units="secs";	i++;
	lstfile[i].isSkipped=false;	lstfile[i].id=0x0011;	lstfile[i].name="REAR PRESSURE";	lstfile[i].datatype="Double";	lstfile[i].units="kg";	i++;
	lstfile[i].isSkipped=false;	lstfile[i].id=0x000F;	lstfile[i].name="FLOOD HEIGHT";	lstfile[i].datatype="Double";	lstfile[i].units="mm";	i++;
	//lstfile[i].isSkipped=false;	lstfile[i].id=0x01C4;	lstfile[i].name="flood_speed";	lstfile[i].datatype="Double";	lstfile[i].units="";	i++;
	lstfile[i].isSkipped=false;	lstfile[i].id=0x0024;	lstfile[i].name="KNEAD BOARDS";	lstfile[i].datatype="Double";	lstfile[i].units="";	i++;
	lstfile[i].isSkipped=false;	lstfile[i].id=0x0022;	lstfile[i].name="PASTE KNEAD PERIOD";	lstfile[i].datatype="Double";	lstfile[i].units="minutes";	i++;
	lstfile[i].isSkipped=false;	lstfile[i].id=0x050C;	lstfile[i].name="PRINT GAP";	lstfile[i].datatype="Double";	lstfile[i].units="mm";	i++;
	lstfile[i].isSkipped=true;	lstfile[i].id=0xFFFF;	lstfile[i].name="SEPARATION DELAY";	lstfile[i].datatype="";	lstfile[i].units="";	i++;
	lstfile[i].isSkipped=false;	lstfile[i].id=0x0013;	lstfile[i].name="SEPARATION SPEED";	lstfile[i].datatype="Double";	lstfile[i].units="mm/s";	i++;
	lstfile[i].isSkipped=false;	lstfile[i].id=0x2715;	lstfile[i].name="SEPARATION DISTANCE";	lstfile[i].datatype="Double";	lstfile[i].units="mm";	i++;
	lstfile[i].isSkipped=false;	lstfile[i].id=0x0048;	lstfile[i].name="UNDER CLEARANCE";	lstfile[i].datatype="Double";	lstfile[i].units="mm";	i++;
	lstfile[i].isSkipped=true;	lstfile[i].id=0xFFFF;	lstfile[i].name="EXTENDED PRINT GAP";	lstfile[i].datatype="";	lstfile[i].units="";	i++;
	lstfile[i].isSkipped=false;	lstfile[i].id=0x0026;	lstfile[i].name="BOARD COUNT";	lstfile[i].datatype="Double";	lstfile[i].units="boards";	i++;
	lstfile[i].isSkipped=true;	lstfile[i].id=0xFFFF;	lstfile[i].name="STINGER HARDWARE";	lstfile[i].datatype="";	lstfile[i].units="";	i++;
	lstfile[i].isSkipped=false;	lstfile[i].id=0x0014;	lstfile[i].name="PRINT MODE";	lstfile[i].datatype="Double[Print/Print,Print/Flood,Flood/Print,Adhesive]";	lstfile[i].units="";	i++;
	lstfile[i].isSkipped=true;	lstfile[i].id=0xFFFF;	lstfile[i].name="PASTE RIDGE REMOVAL";	lstfile[i].datatype="";	lstfile[i].units="";	i++;
	lstfile[i].isSkipped=true;	lstfile[i].id=0xFFFF;	lstfile[i].name="STINGER CAL LOC X";	lstfile[i].datatype="";	lstfile[i].units="";	i++;
	lstfile[i].isSkipped=true;	lstfile[i].id=0xFFFF;	lstfile[i].name="STINGER CAL LOC Y";	lstfile[i].datatype="";	lstfile[i].units="";	i++;
	lstfile[i].isSkipped=true;	lstfile[i].id=0xFFFF;	lstfile[i].name="Stinger Laser Offset Y";	lstfile[i].datatype="";	lstfile[i].units="";	i++;
	lstfile[i].isSkipped=true;	lstfile[i].id=0xFFFF;	lstfile[i].name="Stinger Laser Offset X";	lstfile[i].datatype="";	lstfile[i].units="";	i++;
	lstfile[i].isSkipped=true;	lstfile[i].id=0xFFFF;	lstfile[i].name="Stinger COM Port";	lstfile[i].datatype="";	lstfile[i].units="";	i++;
	lstfile[i].isSkipped=false;	lstfile[i].id=0x0015;	lstfile[i].name="PRINT DEPOSITS";	lstfile[i].datatype="Double";	lstfile[i].units="Prts";	i++;
	lstfile[i].isSkipped=false;	lstfile[i].id=0x001E;	lstfile[i].name="PASTE DISPENSE RATE";	lstfile[i].datatype="Double";	lstfile[i].units="Prts";	i++;
	lstfile[i].isSkipped=false;	lstfile[i].id=0x001F;	lstfile[i].name="PASTE DISPENSE SPEED";	lstfile[i].datatype="Double";	lstfile[i].units="mm/s";	i++;
	lstfile[i].isSkipped=true;	lstfile[i].id=0xFFFF;	lstfile[i].name="PASTE WHILE CLEAN";	lstfile[i].datatype="";	lstfile[i].units="";	i++;
	lstfile[i].isSkipped=true;	lstfile[i].id=0xFFFF;	lstfile[i].name="PASTE WITH BOARD";	lstfile[i].datatype="";	lstfile[i].units="";	i++;
	lstfile[i].isSkipped=true;	lstfile[i].id=0xFFFF;	lstfile[i].name="ALTERNATE DISP";	lstfile[i].datatype="";	lstfile[i].units="";	i++;
	lstfile[i].isSkipped=true;	lstfile[i].id=0xFFFF;	lstfile[i].name="ALTERNATE DISP RATE";	lstfile[i].datatype="";	lstfile[i].units="";	i++;
	lstfile[i].isSkipped=false;	lstfile[i].id=0x0113;	lstfile[i].name="PASTE RECOVERY RATE";	lstfile[i].datatype="Double";	lstfile[i].units="";	i++;
	lstfile[i].isSkipped=false;	lstfile[i].id=0x0111;	lstfile[i].name="FRONT PASTE RECOVERY";	lstfile[i].datatype="Double";	lstfile[i].units="mm";	i++;
	lstfile[i].isSkipped=false;	lstfile[i].id=0x0112;	lstfile[i].name="REAR PASTE RECOVERY";	lstfile[i].datatype="Double";	lstfile[i].units="mm";	i++;
	lstfile[i].isSkipped=true;	lstfile[i].id=0xFFFF;	lstfile[i].name="PASTE RECOVERY SQUEEGEE PRESSURE";	lstfile[i].datatype="";	lstfile[i].units="";	i++;
	lstfile[i].isSkipped=true;	lstfile[i].id=0xFFFF;	lstfile[i].name="BLUE CLEAN MODE 1";	lstfile[i].datatype="";	lstfile[i].units="";	i++;
	lstfile[i].isSkipped=true;	lstfile[i].id=0xFFFF;	lstfile[i].name="Prime Paste Dispenses";	lstfile[i].datatype="";	lstfile[i].units="";	i++;
	lstfile[i].isSkipped=false;	lstfile[i].id=0x0028;	lstfile[i].name="CLEAN RATE 1";	lstfile[i].datatype="Double";	lstfile[i].units="";	i++;
	lstfile[i].isSkipped=true;	lstfile[i].id=0xFFFF;	lstfile[i].name="BLUE CLEAN MODE 2";	lstfile[i].datatype="";	lstfile[i].units="";	i++;
	lstfile[i].isSkipped=false;	lstfile[i].id=0x002A;	lstfile[i].name="CLEAN RATE 2";	lstfile[i].datatype="Double";	lstfile[i].units="";	i++;
	lstfile[i].isSkipped=true;	lstfile[i].id=0xFFFF;	lstfile[i].name="BLUE CLEAN AFTER KNEAD";	lstfile[i].datatype="";	lstfile[i].units="";	i++;
	lstfile[i].isSkipped=true;	lstfile[i].id=0xFFFF;	lstfile[i].name="BLUE CLEAN AFTER DOWNTIME";	lstfile[i].datatype="";	lstfile[i].units="";	i++;
	lstfile[i].isSkipped=true;	lstfile[i].id=0xFFFF;	lstfile[i].name="CLEAN AFTER TIME";	lstfile[i].datatype="";	lstfile[i].units="";	i++;
	lstfile[i].isSkipped=true;	lstfile[i].id=0xFFFF;	lstfile[i].name="BLUE DRY CLEAN SPEED";	lstfile[i].datatype="";	lstfile[i].units="";	i++;
	lstfile[i].isSkipped=true;	lstfile[i].id=0xFFFF;	lstfile[i].name="BLUE WET CLEAN SPEED";	lstfile[i].datatype="";	lstfile[i].units="";	i++;
	lstfile[i].isSkipped=true;	lstfile[i].id=0xFFFF;	lstfile[i].name="BLUE VAC CLEAN SPEED";	lstfile[i].datatype="";	lstfile[i].units="";	i++;
	lstfile[i].isSkipped=true;	lstfile[i].id=0xFFFF;	lstfile[i].name="VAC CLEAN START TIME";	lstfile[i].datatype="";	lstfile[i].units="";	i++;
	lstfile[i].isSkipped=false;	lstfile[i].id=0x75A5;	lstfile[i].name="VACUUM ON PERIOD";	lstfile[i].datatype="Double";	lstfile[i].units="";	i++;
	lstfile[i].isSkipped=true;	lstfile[i].id=0xFFFF;	lstfile[i].name="FRONT START OFFSET";	lstfile[i].datatype="";	lstfile[i].units="";	i++;
	lstfile[i].isSkipped=true;	lstfile[i].id=0xFFFF;	lstfile[i].name="REAR START OFFSET";	lstfile[i].datatype="";	lstfile[i].units="";	i++;
	lstfile[i].isSkipped=false;	lstfile[i].id=0x0023;	lstfile[i].name="KNEAD DEPOSITS";	lstfile[i].datatype="Double";	lstfile[i].units="";	i++;
	lstfile[i].isSkipped=true;	lstfile[i].id=0xFFFF;	lstfile[i].name="KNEAD AFTER DISPENSE";	lstfile[i].datatype="";	lstfile[i].units="";	i++;
	lstfile[i].isSkipped=true;	lstfile[i].id=0xFFFF;	lstfile[i].name="FRONT KNEAD SPEED";	lstfile[i].datatype="";	lstfile[i].units="";	i++;
	lstfile[i].isSkipped=true;	lstfile[i].id=0xFFFF;	lstfile[i].name="REAR KNEAD SPEED";	lstfile[i].datatype="";	lstfile[i].units="";	i++;
	lstfile[i].isSkipped=false;	lstfile[i].id=0x01C5;	lstfile[i].name="REAR KNEAD PRESSURE";	lstfile[i].datatype="Double";	lstfile[i].units="";	i++;
	lstfile[i].isSkipped=false;	lstfile[i].id=0x01C6;	lstfile[i].name="FRONT KNEAD PRESSURE";	lstfile[i].datatype="Double";	lstfile[i].units="";	i++;
	lstfile[i].isSkipped=true;	lstfile[i].id=0xFFFF;	lstfile[i].name="SELECTIVE PRINT";	lstfile[i].datatype="";	lstfile[i].units="";	i++;
	lstfile[i].isSkipped=false;	lstfile[i].id=0x2AF8;	lstfile[i].name="BOARD 1 FID. TYPE";	lstfile[i].datatype="Double[Circle,Rectangle,Diamond,Triangle,Double Square,Cross,Video Model]";	lstfile[i].units="";	i++;
	//lstfile[i].isSkipped=false;	lstfile[i].id=#0x2AF9;	lstfile[i].name="fiducial_1_board_width";	lstfile[i].datatype="Double";	lstfile[i].units="";	i++;
	//lstfile[i].isSkipped=false;	lstfile[i].id=#0x2AFA;	lstfile[i].name="fiducial_1_board_height";	lstfile[i].datatype="Double";	lstfile[i].units="";	i++;
	//lstfile[i].isSkipped=false;	lstfile[i].id=#0x2AFB;	lstfile[i].name="fiducial_1_board_outer_contour";	lstfile[i].datatype="Double";	lstfile[i].units="";	i++;
	//lstfile[i].isSkipped=false;	lstfile[i].id=#0x2AFC;	lstfile[i].name="fiducial_1_board_inner_contour";	lstfile[i].datatype="Double";	lstfile[i].units="";	i++;
	//lstfile[i].isSkipped=false;	lstfile[i].id=#0x2AFD;	lstfile[i].name="fiducial_1_board_rounding";	lstfile[i].datatype="Double";	lstfile[i].units="";	i++;
	//lstfile[i].isSkipped=false;	lstfile[i].id=#0x2AFE;	lstfile[i].name="fiducial_1_board_rotation";	lstfile[i].datatype="Double";	lstfile[i].units="";	i++;
	//lstfile[i].isSkipped=false;	lstfile[i].id=#0x2B00;	lstfile[i].name="fiducial_1_board_score";	lstfile[i].datatype="Double";	lstfile[i].units="";	i++;
	//lstfile[i].isSkipped=false;	lstfile[i].id=#0x2B02;	lstfile[i].name="fiducial_1_board_background";	lstfile[i].datatype="Double";	lstfile[i].units="";	i++;
	lstfile[i].isSkipped=false;	lstfile[i].id=0x2BC0;	lstfile[i].name="BOARD 2 FID. TYPE";	lstfile[i].datatype="Double[Circle,Rectangle,Diamond,Triangle,Double Square,Cross,Video Model]";	lstfile[i].units="";	i++;
	//lstfile[i].isSkipped=false;	lstfile[i].id=#0x2BC1;	lstfile[i].name="fiducial_2_board_width";	lstfile[i].datatype="Double";	lstfile[i].units="";	i++;
	//lstfile[i].isSkipped=false;	lstfile[i].id=#0x2BC2;	lstfile[i].name="fiducial_2_board_height";	lstfile[i].datatype="Double";	lstfile[i].units="";	i++;
	//lstfile[i].isSkipped=false;	lstfile[i].id=#0x2BC3;	lstfile[i].name="fiducial_2_board_outer_contour";	lstfile[i].datatype="Double";	lstfile[i].units="";	i++;
	//lstfile[i].isSkipped=false;	lstfile[i].id=#0x2BC4;	lstfile[i].name="fiducial_2_board_inner_contour";	lstfile[i].datatype="Double";	lstfile[i].units="";	i++;
	//lstfile[i].isSkipped=false;	lstfile[i].id=#0x2BC5;	lstfile[i].name="fiducial_2_board_rounding";	lstfile[i].datatype="Double";	lstfile[i].units="";	i++;
	//lstfile[i].isSkipped=false;	lstfile[i].id=#0x2BC6;	lstfile[i].name="fiducial_2_board_rotation";	lstfile[i].datatype="Double";	lstfile[i].units="";	i++;
	//lstfile[i].isSkipped=false;	lstfile[i].id=#0x2BC8;	lstfile[i].name="fiducial_2_board_score";	lstfile[i].datatype="Double";	lstfile[i].units="";	i++;
	//lstfile[i].isSkipped=false;	lstfile[i].id=#0x2BCA;	lstfile[i].name="fiducial_2_board_background";	lstfile[i].datatype="Double";	lstfile[i].units="";	i++;
	lstfile[i].isSkipped=false;	lstfile[i].id=0x2C88;	lstfile[i].name="BOARD 3 FID. TYPE";	lstfile[i].datatype="Double[Circle,Rectangle,Diamond,Triangle,Double Square,Cross,Video Model]";	lstfile[i].units="";	i++;
	//lstfile[i].isSkipped=false;	lstfile[i].id=#0x2C89;	lstfile[i].name="fiducial_3_board_width";	lstfile[i].datatype="Double";	lstfile[i].units="";	i++;
	//lstfile[i].isSkipped=false;	lstfile[i].id=#0x2C8A;	lstfile[i].name="fiducial_3_board_height";	lstfile[i].datatype="Double";	lstfile[i].units="";	i++;
	//lstfile[i].isSkipped=false;	lstfile[i].id=#0x2C8B;	lstfile[i].name="fiducial_3_board_outer_contour";	lstfile[i].datatype="Double";	lstfile[i].units="";	i++;
	//lstfile[i].isSkipped=false;	lstfile[i].id=#0x2C8C;	lstfile[i].name="fiducial_3_board_inner_contour";	lstfile[i].datatype="Double";	lstfile[i].units="";	i++;
	//lstfile[i].isSkipped=false;	lstfile[i].id=#0x2C8D;	lstfile[i].name="fiducial_3_board_rounding";	lstfile[i].datatype="Double";	lstfile[i].units="";	i++;
	//lstfile[i].isSkipped=false;	lstfile[i].id=#0x2C8E;	lstfile[i].name="fiducial_3_board_rotation";	lstfile[i].datatype="Double";	lstfile[i].units="";	i++;
	//lstfile[i].isSkipped=false;	lstfile[i].id=#0x2C90;	lstfile[i].name="fiducial_3_board_score";	lstfile[i].datatype="Double";	lstfile[i].units="";	i++;
	//lstfile[i].isSkipped=false;	lstfile[i].id=#0x2C92;	lstfile[i].name="fiducial_3_board_background";	lstfile[i].datatype="Double";	lstfile[i].units="";	i++;
	lstfile[i].isSkipped=false;	lstfile[i].id=0x2B5C;	lstfile[i].name="SCREEN 1 FID. TYPE";	lstfile[i].datatype="Double[Circle,Rectangle,Diamond,Triangle,Double Square,Cross,Video Model]";	lstfile[i].units="";	i++;
	//lstfile[i].isSkipped=false;	lstfile[i].id=#0x2B5D;	lstfile[i].name="fiducial_1_screen_width";	lstfile[i].datatype="Double";	lstfile[i].units="";	i++;
	//lstfile[i].isSkipped=false;	lstfile[i].id=#0x2B5E;	lstfile[i].name="fiducial_1_screen_height";	lstfile[i].datatype="Double";	lstfile[i].units="";	i++;
	//lstfile[i].isSkipped=false;	lstfile[i].id=#0x2B5F;	lstfile[i].name="fiducial_1_screen_outer_contour";	lstfile[i].datatype="Double";	lstfile[i].units="";	i++;
	//lstfile[i].isSkipped=false;	lstfile[i].id=#0x2B60;	lstfile[i].name="fiducial_1_screen_inner_contour";	lstfile[i].datatype="Double";	lstfile[i].units="";	i++;
	//lstfile[i].isSkipped=false;	lstfile[i].id=#0x2B61;	lstfile[i].name="fiducial_1_screen_rounding";	lstfile[i].datatype="Double";	lstfile[i].units="";	i++;
	//lstfile[i].isSkipped=false;	lstfile[i].id=#0x2B62;	lstfile[i].name="fiducial_1_screen_rotation";	lstfile[i].datatype="Double";	lstfile[i].units="";	i++;
	//lstfile[i].isSkipped=false;	lstfile[i].id=#0x2B64;	lstfile[i].name="fiducial_1_screen_score";	lstfile[i].datatype="Double";	lstfile[i].units="";	i++;
	//lstfile[i].isSkipped=false;	lstfile[i].id=#0x2B66;	lstfile[i].name="fiducial_1_screen_background";	lstfile[i].datatype="Double";	lstfile[i].units="";	i++;
	lstfile[i].isSkipped=false;	lstfile[i].id=0x2C24;	lstfile[i].name="SCREEN 2 FID. TYPE";	lstfile[i].datatype="Double[Circle,Rectangle,Diamond,Triangle,Double Square,Cross,Video Model]";	lstfile[i].units="";	i++;
	//lstfile[i].isSkipped=false;	lstfile[i].id=#0x2C25;	lstfile[i].name="fiducial_2_screen_width";	lstfile[i].datatype="Double";	lstfile[i].units="";	i++;
	//lstfile[i].isSkipped=false;	lstfile[i].id=#0x2C26;	lstfile[i].name="fiducial_2_screen_height";	lstfile[i].datatype="Double";	lstfile[i].units="";	i++;
	//lstfile[i].isSkipped=false;	lstfile[i].id=#0x2C27;	lstfile[i].name="fiducial_2_screen_outer_contour";	lstfile[i].datatype="Double";	lstfile[i].units="";	i++;
	//lstfile[i].isSkipped=false;	lstfile[i].id=#0x2C28;	lstfile[i].name="fiducial_2_screen_inner_contour";	lstfile[i].datatype="Double";	lstfile[i].units="";	i++;
	//lstfile[i].isSkipped=false;	lstfile[i].id=#0x2C29;	lstfile[i].name="fiducial_2_screen_rounding";	lstfile[i].datatype="Double";	lstfile[i].units="";	i++;
	//lstfile[i].isSkipped=false;	lstfile[i].id=#0x2C2A;	lstfile[i].name="fiducial_2_screen_rotation";	lstfile[i].datatype="Double";	lstfile[i].units="";	i++;
	//lstfile[i].isSkipped=false;	lstfile[i].id=#0x2C2C;	lstfile[i].name="fiducial_2_screen_score";	lstfile[i].datatype="Double";	lstfile[i].units="";	i++;
	//lstfile[i].isSkipped=false;	lstfile[i].id=#0x2C2E;	lstfile[i].name="fiducial_2_screen_background";	lstfile[i].datatype="Double";	lstfile[i].units="";	i++;
	lstfile[i].isSkipped=false;	lstfile[i].id=0x2CEC;	lstfile[i].name="SCREEN 3 FID. TYPE";	lstfile[i].datatype="Double[Circle,Rectangle,Diamond,Triangle,Double Square,Cross,Video Model]";	lstfile[i].units="";	i++;
	//lstfile[i].isSkipped=false;	lstfile[i].id=#0x2CED;	lstfile[i].name="fiducial_3_screen_width";	lstfile[i].datatype="Double";	lstfile[i].units="";	i++;
	//lstfile[i].isSkipped=false;	lstfile[i].id=#0x2CEE;	lstfile[i].name="fiducial_3_screen_height";	lstfile[i].datatype="Double";	lstfile[i].units="";	i++;
	//lstfile[i].isSkipped=false;	lstfile[i].id=#0x2CEF;	lstfile[i].name="fiducial_3_screen_outer_contour";	lstfile[i].datatype="Double";	lstfile[i].units="";	i++;
	//lstfile[i].isSkipped=false;	lstfile[i].id=#0x2CF0;	lstfile[i].name="fiducial_3_screen_inner_contour";	lstfile[i].datatype="Double";	lstfile[i].units="";	i++;
	//lstfile[i].isSkipped=false;	lstfile[i].id=#0x2CF1;	lstfile[i].name="fiducial_3_screen_rounding";	lstfile[i].datatype="Double";	lstfile[i].units="";	i++;
	//lstfile[i].isSkipped=false;	lstfile[i].id=#0x2CF2;	lstfile[i].name="fiducial_3_screen_rotation";	lstfile[i].datatype="Double";	lstfile[i].units="";	i++;
	//lstfile[i].isSkipped=false;	lstfile[i].id=#0x2CF4;	lstfile[i].name="fiducial_3_screen_score";	lstfile[i].datatype="Double";	lstfile[i].units="";	i++;
	//lstfile[i].isSkipped=false;	lstfile[i].id=#0x2CF6;	lstfile[i].name="fiducial_3_screen_background";	lstfile[i].datatype="Double";	lstfile[i].units="";	i++;
	lstfile[i].isSkipped=false;	lstfile[i].id=0x0033;	lstfile[i].name="FIDUCIAL 1 X";	lstfile[i].datatype="Double";	lstfile[i].units="";	i++;
	lstfile[i].isSkipped=false;	lstfile[i].id=0x0034;	lstfile[i].name="FIDUCIAL 1 Y";	lstfile[i].datatype="Double";	lstfile[i].units="";	i++;
	lstfile[i].isSkipped=false;	lstfile[i].id=0x0035;	lstfile[i].name="FIDUCIAL 2 X";	lstfile[i].datatype="Double";	lstfile[i].units="";	i++;
	lstfile[i].isSkipped=false;	lstfile[i].id=0x0036;	lstfile[i].name="FIDUCIAL 2 Y";	lstfile[i].datatype="Double";	lstfile[i].units="";	i++;
	lstfile[i].isSkipped=false;	lstfile[i].id=0x0037;	lstfile[i].name="FIDUCIAL 3 X";	lstfile[i].datatype="Double";	lstfile[i].units="";	i++;
	lstfile[i].isSkipped=false;	lstfile[i].id=0x0038;	lstfile[i].name="FIDUCIAL 3 Y";	lstfile[i].datatype="Double";	lstfile[i].units="";	i++;
	lstfile[i].isSkipped=false;	lstfile[i].id=0x0039;	lstfile[i].name="FORWARD X OFFSET";	lstfile[i].datatype="Double";	lstfile[i].units="";	i++;
	lstfile[i].isSkipped=false;	lstfile[i].id=0x003A;	lstfile[i].name="FORWARD Y OFFSET";	lstfile[i].datatype="Double";	lstfile[i].units="";	i++;
	lstfile[i].isSkipped=false;	lstfile[i].id=0x003B;	lstfile[i].name="FORWARD W OFFSET";	lstfile[i].datatype="Double";	lstfile[i].units="";	i++;
	lstfile[i].isSkipped=false;	lstfile[i].id=0x003C;	lstfile[i].name="REVERSE X OFFSET";	lstfile[i].datatype="Double";	lstfile[i].units="";	i++;
	lstfile[i].isSkipped=false;	lstfile[i].id=0x003D;	lstfile[i].name="REVERSE Y OFFSET";	lstfile[i].datatype="Double";	lstfile[i].units="";	i++;
	lstfile[i].isSkipped=false;	lstfile[i].id=0x003E;	lstfile[i].name="REVERSE W OFFSET";	lstfile[i].datatype="Double";	lstfile[i].units="";	i++;
	lstfile[i].isSkipped=true;	lstfile[i].id=0xFFFF;	lstfile[i].name="VIEW ROUGH ALIGNMENT";	lstfile[i].datatype="";	lstfile[i].units="";	i++;
	lstfile[i].isSkipped=false;	lstfile[i].id=0x0042;	lstfile[i].name="ALIGNMENT WEIGHTING";	lstfile[i].datatype="Double";	lstfile[i].units="";	i++;
	lstfile[i].isSkipped=false;	lstfile[i].id=0x0043;	lstfile[i].name="X ALIGN WEIGHTING";	lstfile[i].datatype="Double";	lstfile[i].units="";	i++;
	lstfile[i].isSkipped=false;	lstfile[i].id=0x0044;	lstfile[i].name="Y ALIGN WEIGHTING";	lstfile[i].datatype="Double";	lstfile[i].units="";	i++;
	lstfile[i].isSkipped=false;	lstfile[i].id=0x0032;	lstfile[i].name="ALIGNMENT MODE";	lstfile[i].datatype="Double[2 FIDUCIAL,3 FIDUCIAL]";	lstfile[i].units="";	i++;
	lstfile[i].isSkipped=true;	lstfile[i].id=0xFFFF;	lstfile[i].name="TOOLING TYPE";	lstfile[i].datatype="";	lstfile[i].units="";	i++;
	lstfile[i].isSkipped=true;	lstfile[i].id=0xFFFF;	lstfile[i].name="Double Align Mode";	lstfile[i].datatype="";	lstfile[i].units="";	i++;
	lstfile[i].isSkipped=true;	lstfile[i].id=0xFFFF;	lstfile[i].name="BOARD-AT-STOP RUN-ON";	lstfile[i].datatype="";	lstfile[i].units="";	i++;
	lstfile[i].isSkipped=false;	lstfile[i].id=0x0046;	lstfile[i].name="BOARD STOP X";	lstfile[i].datatype="Double";	lstfile[i].units="";	i++;
	lstfile[i].isSkipped=false;	lstfile[i].id=0x0047;	lstfile[i].name="BOARD STOP Y";	lstfile[i].datatype="Double";	lstfile[i].units="";	i++;
	lstfile[i].isSkipped=false;	lstfile[i].id=0x0020;	lstfile[i].name="PASTE START";	lstfile[i].datatype="Double";	lstfile[i].units="";	i++;
	lstfile[i].isSkipped=false;	lstfile[i].id=0x0021;	lstfile[i].name="PASTE STOP";	lstfile[i].datatype="Double";	lstfile[i].units="";	i++;
	//lstfile[i].isSkipped=false;	lstfile[i].id=#0x04BC;	lstfile[i].name="snugging_pressure";	lstfile[i].datatype="double";	lstfile[i].units="";	i++;
	//lstfile[i].isSkipped=false;	lstfile[i].id=#0x04BD;	lstfile[i].name="clamp_pressure";	lstfile[i].datatype="double";	lstfile[i].units="";	i++;
	//lstfile[i].isSkipped=false;	lstfile[i].id=#0x04BE;	lstfile[i].name="ots_retain_blade";	lstfile[i].datatype="double";	lstfile[i].units="";	i++;
	lstfile[i].isSkipped=true;	lstfile[i].id=0xFFFF;	lstfile[i].name="SPC CONFIGURATION";	lstfile[i].datatype="";	lstfile[i].units="";	i++;
	lstfile[i].isSkipped=true;	lstfile[i].id=0xFFFF;	lstfile[i].name="PRESS DECAY INTERVAL";	lstfile[i].datatype="";	lstfile[i].units="";	i++;
	lstfile[i].isSkipped=true;	lstfile[i].id=0xFFFF;	lstfile[i].name="Snugging Force";	lstfile[i].datatype="";	lstfile[i].units="";	i++;
	lstfile[i].isSkipped=true;	lstfile[i].id=0xFFFF;	lstfile[i].name="BStop Retract Delay";	lstfile[i].datatype="";	lstfile[i].units="";	i++;
	//lstfile[i].isSkipped=false;	lstfile[i].id=#0x36B7;	lstfile[i].name="mesh_front";	lstfile[i].datatype="double";	lstfile[i].units="";	i++;
	//lstfile[i].isSkipped=false;	lstfile[i].id=#0x36B8;	lstfile[i].name="mesh_rear";	lstfile[i].datatype="double";	lstfile[i].units="";	i++;
	//lstfile[i].isSkipped=false;	lstfile[i].id=#0x36B9;	lstfile[i].name="mesh_left";	lstfile[i].datatype="double";	lstfile[i].units="";	i++;
	//lstfile[i].isSkipped=false;	lstfile[i].id=#0x36BA;	lstfile[i].name="mesh_right";	lstfile[i].datatype="double";	lstfile[i].units="";	i++;
	//lstfile[i].isSkipped=false;	lstfile[i].id=#0x006E;	lstfile[i].name="dist_to_image";	lstfile[i].datatype="double";	lstfile[i].units="";	i++;
	//return lstfile;
	//delete[] lstfile;
	}
void compare_csv (string file1, string file2, string outpath = "") {
	long count1 = 0;
	long count2 = 0;
	long count3 = 0;
	int i = 0;
	int header_count = 0;
	int sc1 = 0;
	int sc2 = 0;
	string* header;
	string* ss1;
	string* ss2;
	string line;
	string* lineArray;
	csvdiff* diffArray;
	ifstream myfile (file1.c_str());
	if (myfile.is_open()) {
		while (getline(myfile, line)) {
			++count1;
			}
		myfile.clear();
		myfile.seekg (0, ios::beg);
		if (count1 > 0) {
			lineArray = new string[count1];
			for (i = 0; i < count1; i++) {
				getline(myfile, line);
				lineArray[i] = line;
				if (i == 0) {
					split(line, ",", header_count, header);
					}
				}
			myfile.clear();
			myfile.close();
			ifstream myfile (file2.c_str());
			if (myfile.is_open()) {
				while (getline(myfile, line)) {
					++count2;
					if (count2 <= count1) {
						if (lineArray[count2-1] != line) {

							split(lineArray[count2-1], ",", sc1, ss1);
							split(line, ",", sc2, ss2);
							if (sc1 == sc2) {
								for (i = 0; i < sc1; i++) {
									if (ss1[i] != ss2[i]) {
										++count3;
										}
									}
								}
							else {
								++count3;
								}
							sc1 = 0;
							sc2 = 0;
							delete[] ss1;
							delete[] ss2;
							}
						}
					else {
						++count3;
						}
					}
				if (count3 > 0) {
					diffArray = new csvdiff[count3];
					myfile.clear();
					myfile.seekg (0, myfile.beg);
					count2 = 0;
					count3 = 0;
					while (getline(myfile, line)) {
						++count2;
						if (count2 <= count1) {
							if (lineArray[count2-1] != line) {
								split(lineArray[count2-1], ",", sc1, ss1);
								split(line, ",", sc2, ss2);
								if (sc1 == sc2) {
									for (i = 0; i < sc1; i++) {
										if (ss1[i] != ss2[i]) {
											diffArray[count3].MODEL = filenameFromPath(file1);
											diffArray[count3].FIELD = header[i];
											diffArray[count3].FROM = ss1[i];
											diffArray[count3].TO = ss2[i];
											diffArray[count3].TIMESTAMP = get_part_date("%Y-%m-%d %I:%M%p");
											++count3;
											}
										}

									}
								else {
									diffArray[count3].MODEL = filenameFromPath(file1);
									diffArray[count3].FIELD = "";
									diffArray[count3].FROM = "";
									diffArray[count3].TO = "";
									diffArray[count3].TIMESTAMP = get_part_date("%Y-%m-%d %I:%M%p");
									++count3;
									}
								sc1 = 0;
								sc2 = 0;
								delete[] ss1;
								delete[] ss2;
								}
							}
						else {
							diffArray[count3].MODEL = filenameFromPath(file1);
							diffArray[count3].FIELD = "";
							diffArray[count3].FROM = "";
							diffArray[count3].TO = "";
							diffArray[count3].TIMESTAMP = get_part_date("%Y-%m-%d %I:%M%p");
							++count3;
							}
						}
					//delete[] diffArray;
					}
				myfile.clear();
				myfile.close();
				}
			delete[] lineArray;
			}
		if (count3 > 0) {
            if (outpath != "") {
                ofstream writeout;
                writeout.open(outpath.c_str());
                writeout << "TIMESTAMP,MODEL,FIELD,FROM,TO" << endl;
                for (i = 0; i < count3; i++) {
                    writeout <<
                        diffArray[i].TIMESTAMP <<
                        "," <<
                        diffArray[i].MODEL <<
                        "," <<
                        diffArray[i].FIELD <<
                        "," <<
                        diffArray[i].FROM <<
                        "," <<
                        diffArray[i].TO <<
                        endl;
                    }
				writeout.close();
                }
            else {
                cout << "TIMESTAMP,MODEL,FIELD,FROM,TO" << endl;
                for (i = 0; i < count3; i++) {
                    cout <<
                        diffArray[i].TIMESTAMP <<
                        "," <<
                        diffArray[i].MODEL <<
                        "," <<
                        diffArray[i].FIELD <<
                        "," <<
                        diffArray[i].FROM <<
                        "," <<
                        diffArray[i].TO <<
                        endl;
                    }
                }
			delete[] diffArray;
			}
		}
	else {
		cout << "Can't open file"<<endl;
		}
	delete[] header;
	}
void compare_txt (string file1, string file2, string outpath = "") {
	long count1 = 0;
	long count2 = 0;
	long count3 = 0;
	bool hasDebugHeader = false;
	int i = 0;
	string line;
	string* lineArray;
	csvdiff* diffArray;
	ifstream myfile (file1.c_str());

	if (myfile.is_open()) {
		// check if files open
		while (getline(myfile, line)) {
			// uh, probably most inefficient but I pass through the file read each line
			// just to count how many lines.
			if (hasDebugHeader == false && line == "ID      DATATYPE    DATASIZE    NAME                            DATA                            ") {
				hasDebugHeader = true;
				}
			++count1;
			}
		myfile.clear();
		myfile.seekg (0, ios::beg);
		// clear end of stream flag, then go back to start
		if (count1 > 0) {
			// if there was lines counted
			lineArray = new string[count1];
			// then create an array with NEW operator
			for (i = 0; i < count1; i++) {
				getline(myfile, line);
				// read each line of the file again
				lineArray[i] = line;
				// store string to the array
				}
			myfile.clear();
			myfile.close();
			// kill file access to the first file
			ifstream myfile (file2.c_str());
			// open second file
			if (myfile.is_open()) {
				// second file is opened nested, probably bad ? I donno
				while (getline(myfile, line)) {
					++count2;
					// yeah we counting this AGAIN, cause I don't wanna
					// waste another pre sweep to check if each file
					// has the same amount of lines.
					// so actively keep a counter and check if we over run or not...
					if (count2 <= count1) {
						// if not over run continue the hunt
						if (lineArray[count2-1] != line) {
							// string actively read from the file, is then
							// string compared with the string stored in the
							// array.
							// keep it simple do the order of the lines
							// must be the same.
							// it would suck if I had to compare if lines
							// match and which don't...
							// well I guess it wouldn't be that fair
							// off from what I'm already doing....
							// oh wells...
							++count3;
							// if shit went bad, then add it to the count...
							// yes this is actually a pre pass for another array
							// probably very inefficient :(
							// good thing the files are small ;) mehahaha!
							}
						}
					else {
						++count3;
						// if the lines seq don't match count it
						// this is a prepass, the data gets apppened later.
						}
					}
				if (count3 > 0) {
					// count3 should be 0 if all was good
					// if there's a positive number then
					// well shit gotta read the file again :(
					diffArray = new csvdiff[count3];
					// create new array using NEW operator
					myfile.clear();
					myfile.seekg (0, myfile.beg);
					// reset EOS flag, then seek back to start of file
					count2 = 0;
					count3 = 0;
					// reset these counts, cause why not
					while (getline(myfile, line)) {
						// I should have tagged the lines that needed checking,
						// but i donno nothing,
						// soooo we just read the file again, most inefficient probably..
						++count2;
						// keep count, check for over run.
						if (count2 <= count1) {
							// no over run, great check if lines match
							if (lineArray[count2-1] != line) {
								// crap lines didnt match
								if (hasDebugHeader) {
									diffArray[count3].TIMESTAMP = get_part_date("%Y-%m-%d %I:%M%p");
									diffArray[count3].MODEL = filenameFromPath(file1);
									if (lineArray[count2-1].size() > 64) {
										diffArray[count3].FROM = lineArray[count2-1].substr(64, lineArray[count2-1].size() - 64);
										}
									if (line.size() > 64) {
										diffArray[count3].TO = line.substr(64, line.size() - 64);
										}
									if (line.size() >= 64) {
										diffArray[count3].FIELD = rtrim(line.substr(32, 32));
										}
									}
								else {
									diffArray[count3].MODEL = filenameFromPath(file1);
									diffArray[count3].FROM = lineArray[count2-1];
									diffArray[count3].FIELD = "";
									diffArray[count3].TO = line;
									diffArray[count3].TIMESTAMP = get_part_date("%Y-%m-%d %I:%M%p");
									}
								++count3;
								}
							}
						else {
							// there was an over run
							diffArray[count3].MODEL = filenameFromPath(file1);
							//diffArray[count3].FIELD = "";
							diffArray[count3].FROM = "";
							diffArray[count3].TO = "";
							diffArray[count3].TIMESTAMP = get_part_date("%Y-%m-%d %I:%M%p");
							++count3;
							}
						}
					}
				// kill second file, wee dooone...!
				myfile.clear();
				myfile.close();
				}
			delete[] lineArray;
			// that first file that we stored to a string
			// array can be freed from memory now.
			}
		if (count3 > 0) {
            if (outpath != "") {
                ofstream writeout;
                writeout.open(outpath.c_str());
                writeout << "TIMESTAMP,MODEL,FIELD,FROM,TO" << endl;
                for (i = 0; i < count3; i++) {
                    writeout <<
                        diffArray[i].TIMESTAMP <<
                        "," <<
                        diffArray[i].MODEL <<
                        "," <<
                        diffArray[i].FIELD <<
                        "," <<
                        diffArray[i].FROM <<
                        "," <<
                        diffArray[i].TO <<
                        endl;
                    }
				writeout.close();
                }
            else {
                cout << "TIMESTAMP,MODEL,FIELD,FROM,TO" << endl;
                for (i = 0; i < count3; i++) {
                    cout <<
                        diffArray[i].TIMESTAMP <<
                        "," <<
                        diffArray[i].MODEL <<
                        "," <<
                        diffArray[i].FIELD <<
                        "," <<
                        diffArray[i].FROM <<
                        "," <<
                        diffArray[i].TO <<
                        endl;
                    }
                }
			delete[] diffArray;
			}
		}
	else {

		cout << "Can't open file"<<endl;
		}
	}
string subTokens (string path) {
	path = ReplaceString(path, "%Y", (string)get_part_date("%Y"));
	path = ReplaceString(path, "%m", (string)get_part_date("%m"));
	path = ReplaceString(path, "%d", (string)get_part_date("%d"));
	path = ReplaceString(path, "%H", (string)get_part_date("%H"));
	path = ReplaceString(path, "%M", (string)get_part_date("%M"));
	path = ReplaceString(path, "%S", (string)get_part_date("%S"));
	return path;
	}
/// SHA256
const unsigned int SHA256::sha256_k[64] = { //UL = uint32
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
    0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
    0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
    };
void SHA256::transform(const unsigned char *message, unsigned int block_nb) {
    uint32 w[64];
    uint32 wv[8];
    uint32 t1, t2;
    const unsigned char *sub_block;
    int i;
    int j;
    for (i = 0; i < (int) block_nb; i++) {
        sub_block = message + (i << 6);
        for (j = 0; j < 16; j++) {
            SHA2_PACK32(&sub_block[j << 2], &w[j]);
        }
        for (j = 16; j < 64; j++) {
            w[j] =  SHA256_F4(w[j -  2]) + w[j -  7] + SHA256_F3(w[j - 15]) + w[j - 16];
        }
        for (j = 0; j < 8; j++) {
            wv[j] = m_h[j];
        }
        for (j = 0; j < 64; j++) {
            t1 = wv[7] + SHA256_F2(wv[4]) + SHA2_CH(wv[4], wv[5], wv[6])
                + sha256_k[j] + w[j];
            t2 = SHA256_F1(wv[0]) + SHA2_MAJ(wv[0], wv[1], wv[2]);
            wv[7] = wv[6];
            wv[6] = wv[5];
            wv[5] = wv[4];
            wv[4] = wv[3] + t1;
            wv[3] = wv[2];
            wv[2] = wv[1];
            wv[1] = wv[0];
            wv[0] = t1 + t2;
        }
        for (j = 0; j < 8; j++) {
            m_h[j] += wv[j];
        }
    }
}
void SHA256::init() {
    m_h[0] = 0x6a09e667;
    m_h[1] = 0xbb67ae85;
    m_h[2] = 0x3c6ef372;
    m_h[3] = 0xa54ff53a;
    m_h[4] = 0x510e527f;
    m_h[5] = 0x9b05688c;
    m_h[6] = 0x1f83d9ab;
    m_h[7] = 0x5be0cd19;
    m_len = 0;
    m_tot_len = 0;
}
void SHA256::update(const unsigned char *message, unsigned int len) {
    unsigned int block_nb;
    unsigned int new_len, rem_len, tmp_len;
    const unsigned char *shifted_message;
    tmp_len = SHA224_256_BLOCK_SIZE - m_len;
    rem_len = len < tmp_len ? len : tmp_len;
    memcpy(&m_block[m_len], message, rem_len);
    if (m_len + len < SHA224_256_BLOCK_SIZE) {
        m_len += len;
        return;
    }
    new_len = len - rem_len;
    block_nb = new_len / SHA224_256_BLOCK_SIZE;
    shifted_message = message + rem_len;
    transform(m_block, 1);
    transform(shifted_message, block_nb);
    rem_len = new_len % SHA224_256_BLOCK_SIZE;
    memcpy(m_block, &shifted_message[block_nb << 6], rem_len);
    m_len = rem_len;
    m_tot_len += (block_nb + 1) << 6;
}
void SHA256::final(unsigned char *digest) {
    unsigned int block_nb;
    unsigned int pm_len;
    unsigned int len_b;
    int i;
    block_nb = (1 + ((SHA224_256_BLOCK_SIZE - 9)
                     < (m_len % SHA224_256_BLOCK_SIZE)));
    len_b = (m_tot_len + m_len) << 3;
    pm_len = block_nb << 6;
    memset(m_block + m_len, 0, pm_len - m_len);
    m_block[m_len] = 0x80;
    SHA2_UNPACK32(len_b, m_block + pm_len - 4);
    transform(m_block, block_nb);
    for (i = 0 ; i < 8; i++) {
        SHA2_UNPACK32(m_h[i], &digest[i << 2]);
    }
}
std::string sha256(std::string input) {
    unsigned char digest[SHA256::DIGEST_SIZE];
    memset(digest,0,SHA256::DIGEST_SIZE);

    SHA256 ctx = SHA256();
    ctx.init();
    ctx.update( (unsigned char*)input.c_str(), input.length());
    ctx.final(digest);

    char buf[2*SHA256::DIGEST_SIZE+1];
    buf[2*SHA256::DIGEST_SIZE] = 0;
    for (int i = 0; i < SHA256::DIGEST_SIZE; i++)
        sprintf(buf+i*2, "%02x", digest[i]);
    return std::string(buf);
}
/// END SHA256


/// simple file watcher
class UpdateListener : public FW::FileWatchListener {
public:
	std::string** pr1files;
	lst_data* lstfile;
	int wait_time = 0;
	int success_count = 0;
	UpdateListener() {}
	void handleFileAction(FW::WatchID watchid, const FW::String& dir, const FW::String& filename, FW::Action action) {
		//std::cout << "DIR (" << dir + ") FILE (" + toupper(getFilenameType(filename)) + ") has event " << action << std::endl;
		std::cout << "Detected File" << std::endl;
		if (toupper(getFilenameType(filename)) == ".PR1") {
			success_count += 1;
			remove((dir + filename + ".temp").c_str());
			if (is_file_exist(dir + filename + ".temp") == false) {
				std::cout << success_count << " Detected PR1 File" << std::endl;
				while (is_file_exist(dir + filename + ".temp") == false) {
					std::cout << "Attempt to Rename\n\t" << (dir + filename, dir + filename) << std::endl;
					rename((dir + filename, dir + filename).c_str(), (dir + filename, dir + filename + ".temp").c_str());
					wait_time += 1000;
					Sleep(wait_time);
					if (wait_time > (1000 * 60)) {
						wait_time = 0;
						std::cout << "error, breaking" << std::endl;
						break;
						}
					}
				std::cout << "File Renamed!" << std::endl;
				if (is_file_exist(dir + filename + ".temp")) {
					stringvec files;
					files.push_back(dir + filename + ".temp");
					readFieldsFromPR1File(pr1files, files, lstfile);
					printCSV (
						pr1files,
						lstfile,
						1,
						dir + getFilenameFile(filename) + ".csv",
						false,
						files
						);
					std::cout << "File Wrote!" << std::endl;
					while (is_file_exist(dir + filename) == false) {
						rename((dir + filename, dir + filename + ".temp").c_str(), (dir + filename, dir + filename).c_str());
						wait_time += 1000;
						Sleep(wait_time);
						if (wait_time > (1000 * 60)) {
							break;
							wait_time = 0;
							std::cout << "failed to clear temp file" << std::endl;
							}
						}
					std::cout << "cleared temp file" << std::endl;
					}
				else {
					std::cout << "Failed to rename file" << std::endl;
					}
				}


			}




		}
	};


int main(int argc, char* argv[]) {
	// variables
	string** pr1files;
	lst_data* lstfile;
	stringvec files;
	bool showHelp = false;
	bool showNotePad = true;
	bool showMsgBox = false;
	bool useTokens = false;
	bool exportTable = false;
	bool exportRAW = false;
	bool importTable = false;
	bool enableWrite = false;
	bool monitorFiles = false;
	bool hideConsole = false;
	bool csvCmp = false;
	bool txtCmp = false;
	string show = "ALL";
	string cmpPath1 = "";
	string cmpPath2 = "";
	string exPath = "";
	string monitorPath = "";
	string savepath = "";
	string dumppath = "";
	string imPath = "";
	string inFolder = "";
	string curFolder = getCurrentPath(argv[0]);
	int btn = IDYES;
	string msgstr = "BETA VERSION v1.0\nONLY for testing and feedback!\n\n\nQuick Warning:\nUse this App at your own risk,\nwon't be responsible for any\ndata loss or damages that may\nhave been a result of using it.\nAgree Yes/No\n\n- Corey\n";

	// Initialize ID Table
	genLST(lstfile);

	inFolder = curFolder;

	// Parse for arguments
	if (argc > 1) {
		// there are arguments or files supplied
		for (int a = 1; a < argc; ++a) {
			// look for arguments
			if ((string)argv[a] == "?") {
				showHelp = true;
				}
			else if ((string)argv[a] == "-nopad") {
				showNotePad = false;
				}
			else if ((string)argv[a] == "-hide") {
				hideConsole = true;
				}
			else if ((string)argv[a] == "-known") {
				show = "KNOWN";
				}
			else if ((string)argv[a] == "-unknown") {
				show = "UNKNOWN";
				}
			else if ((string)argv[a] == "-monitor") {
				if (a < argc) {
					monitorFiles = true;
					monitorPath = argv[a + 1];
					monitorPath.erase(
						remove(monitorPath.begin(), monitorPath.end(), '\"' ),
						monitorPath.end()
						);
					if (monitorPath == ".") {
						monitorPath = curFolder;
						}
					if (monitorPath.length() == 0) {
						monitorPath = curFolder;
						}
					if (monitorPath.substr(monitorPath.length() - 1, 1) != "\\") {
						monitorPath += "\\";
						}
					a++;
					}
				}
			else if ((string)argv[a] == "-cmpcsv") {
				csvCmp = true;
				if (argc >= (a + 2)) {
					cmpPath1 = argv[a + 1];
					cmpPath2 = argv[a + 2];
					a+=2;
					}
				}
			else if ((string)argv[a] == "-cmptxt") {
				txtCmp = true;
				if (argc >= (a + 2)) {
					cmpPath1 = argv[a + 1];
					cmpPath2 = argv[a + 2];
					a+=2;
					}
				}
			else if ((string)argv[a] == "-outfile") {
				if (a < argc) {
					savepath = (string)argv[a + 1];
					a++;
					}
				}
			else if ((string)argv[a] == "-dump") {
				if (a < argc) {
					exportRAW = true;
					dumppath = (string)argv[a + 1];
					a++;
					}
				}
			else if ((string)argv[a] == "-export") {
				if (a < argc) {
					exportTable = true;
					exPath = argv[a + 1];
					a++;
					}
				}
			else if ((string)argv[a] == "-import") {
				if (a < argc) {
					importTable = true;
					imPath = argv[a + 1];
					a++;
					}
				}
			else if ((string)argv[a] == "-folder") {
				if (a < argc) {
					inFolder = argv[a + 1];
					inFolder.erase(
						remove( inFolder.begin(), inFolder.end(), '\"' ),
						inFolder.end()
						);
					if (inFolder == ".") {
						inFolder = curFolder;
						}
					if (inFolder.length() == 0) {
						inFolder = curFolder;
						}
					if (inFolder.substr(inFolder.length() - 1, 1) != "\\") {
						inFolder += "\\";
						}
					read_directory(inFolder, inFolder + "*.pr1", files);
					a++;
					}
				}
			else {
				if (is_file_exist((string)argv[a])) {
					files.push_back(argv[a]);
					}
				else {
					files.push_back(inFolder + argv[a]);
					}
				}
			}
		// when ? found as argument, display following
		if (showHelp) {
			cout << "=======================================================" << endl;
			cout << "  DEK PR1 Tool" << endl;
			cout << "=======================================================" << endl;
			cout << "  Description:" << endl;
			cout << "\tDumps data from PR1 binary" << endl;
			cout << "\tTested with the 'Horizon 03i'" << endl;
			cout << "\tWith software veriosn '09 SP07 P02'\n" << endl;
			cout << "  About:" << endl;
			cout << "\tAuthor:\tCorey Nguyen" << endl;
			cout << "\tDate:\tAugust 18, 2018\n" << endl;
			cout << "  Contributors:" << endl;
			cout << "\tDiego Matias Jacobi\n" << endl;
			cout << "  !Disclaimer!:" << endl;
			cout << "\tUse of this application is at your own risk.\n" << endl;
			cout << "\tDO NOT RUN ON A PRODUCTION MACHINE, MAKE A" << endl;
			cout << "\tCOPY OF YOUR PRINTER PROGRAMS PRIOR TO USE.\n" << endl;
			cout << "\tI will not be held responsible in the event" << endl;
			cout << "\tyou experience data loss or damages." << endl;
			cout << "=======================================================\n" << endl;
			cout << "  Export Options:" << endl;
			cout << "   -nopad\n\t Notepad will not be used to display output\n" << endl;
			cout << "   -outfile <savepath>\n\t specify csv file save path\n" << endl;
			cout << "   -folder <fullpath>\n\t process all in specified folder\n" << endl;
			cout << "   exmaple:" << endl;
			cout << "\t<*.exe> -outfile \"C:\\prg\\mydata_%Y%m%d%H%M%S.csv\"" << endl;
			cout << "\t<*.exe> -nopad \"C:\\prg\\file.pr1\"" << endl;
			cout << "=======================================================\n" << endl;
			cout << "  Debug Options:" << endl;
			cout << "   -show <keyword>\n\tbecause there is data that is not understood" << endl;
			cout << "   \tunknown data can managed to show only what you want" << endl;
			cout << "   \n\tkeywords:\n\t\t-all -known -unknown\n" << endl;
			cout << "   -usetokens\n\tAny savepath to a file will be parsed, if an\n   \toperator is found it will be substituted" << endl;
			cout << "   \n\toperators:\n\t\t%Y = Year, %m = Month, d = day" << endl;
			cout << "   \t\t%H = hour, M = minute, S = second" << endl;
			cout << "   exmaple:\n\t <*.exe> \"C:\\prg\\mydata_%Y%m%d%H%M%S.csv\"" << endl;
			cout << "   \t -> \"C:\\prg\\my_data_" << get_part_date("%Y%m%d%H%M%S") << ".csv\"\n" << endl;
			cout << "   -monitor <path>\n\twatches specified path for any changes to PR1 files" << endl;
			cout << "   \tif PR1 is edited, check for existing related csv file" << endl;
			cout << "   \tif no related csv, create new one and add the changes" << endl;
			cout << "   \telse appened to the related csv\n" << endl;
			cout << "   exmaple:\n\t <*.exe> -monitor \"C:\\prg\\\"" << endl;
			cout << "   \t <*.exe> -monitor \"C:\\prg\\\" -folder \"C:\\temp\\\"" << endl;
			cout << "   \t <*.exe> -monitor -outfile \"C:\\prg\\changes.csv\"\n" << endl;
			cout << "   -hide\n\tadditional command for -monitor, hides console window\n" << endl;
			cout << "   -dump <savepath>\n\t for debugging, dump all ids to file \n" << endl;
			cout << "   -cmpcsv <file1> <file2> <savefile.csv>\n\tcompares two csv files against each other" << endl;
			cout << "   \tFirst line is used as headers, so must be reserved\n" << endl;
			cout << "   -cmptxt <file1> <file2> <savefile.csv>\n\tcompares two txt files against each other\n" << endl;
			cout << "   exmaple:\n\t <*.exe> -cmptxt \"prg1.csv\" \"prg2.csv\" -outfile \"report.csv\"" << endl;
			cout << "   \t <*.exe> -show -unknown -cmptxt \"prg1.csv\" \"prg2.csv\"\n" << endl;
			cout << "   -export <savepath>\n\t Save file with exe's id table\n" << endl;
			cout << "   -import <openpath>\n\t substitute exe's id table with file\n" << endl;
			cout << "   exmaple:\n\t <*.exe> -exlist \"C:\\list.csv\"" << endl;
			cout << "\t <*.exe> -imlist \"C:\\list.csv\" \"C:\\file.pr1\"" << endl;
			cout << "=======================================================\n" << endl;
			//cout << "Folder:\t" << inFolder << endl;
			//cout << "Files:\t" << files.size() << endl;
			}
        else if (txtCmp) {
            if (cmpPath1 != "" && cmpPath2 != "") {
                compare_txt(cmpPath1, cmpPath2, savepath);
                }
            }
        else if (csvCmp) {
            if (cmpPath1 != "" && cmpPath2 != "") {
                compare_csv(cmpPath1, cmpPath2, savepath);
                }
            }
		else {
			if (showMsgBox) {
				int btn = MessageBox( 0, msgstr.c_str() ,  "About" ,MB_YESNO + MB_ICONQUESTION );
				}
			if ( btn == IDYES ) {
				if (importTable) {
					// replace id table from file
					lstfile = readLST(imPath, lstfile);
					}
				if (exportTable) {
					// save id table template
					if (useTokens) {
						printLST(subTokens(exPath), showNotePad);
						}
					else {
						printLST(exPath, showNotePad);
						}
					printLST(exPath, showNotePad);
					goto finishup;
					}
				if (inFolder == "") {
						// i think this isn't used cause that var will ever be empty?
					inFolder = getFilenamePath((string)argv[1]);
					if (inFolder == "") {
						inFolder = curFolder;
						}
					}
				if (monitorFiles) {
					/// simple file watcher
					try {

						// create the listener (before the file watcher - so it gets destroyed after the file watcher)
						UpdateListener listener;
						listener.lstfile = lstfile;
						listener.pr1files = pr1files;
						// create the file watcher object
						FW::FileWatcher fileWatcher;

						// add a watch to the system
						// the file watcher doesn't manage the pointer to the listener - so make sure you don't just
						// allocate a listener here and expect the file watcher to manage it - there will be a leak!
						if (hideConsole) {
							for(;;){
								HWND hide;
								AllocConsole();
								hide = FindWindowA("ConsoleWindowClass",NULL);
								ShowWindow(hide,0);
								}
							}
						FW::WatchID watchID = fileWatcher.addWatch(monitorPath, &listener, true);

						std::cout << "\n\n\n\n\n\n\n" << endl;
						std::cout << "Current Directory is being monitored for changes\nPress ^C to exit" << std::endl;

						// loop until a key is pressed
						while(1) {
							fileWatcher.update();
							}
						}
					catch( std::exception& e ) {
						fprintf(stderr, "An exception has occurred: %s\n", e.what());
						}
					goto finishup;
					}
				if (files.size() == 0) {
					read_directory(inFolder, "*.pr1", files);
					}
				if (exportRAW) {
					if (files.size() > 0) {
						if (files.size() == 1) {
							if (useTokens) {
								if (dumppath != "") {
									for (int i = 0; i < files.size(); i++) {
										printRAW (
											files[i],
											(dumppath + subTokens(getFilenameFile(files[i])) + "_debug.txt"),
											lstfile,
											showNotePad
											);
										}
									}
								else {
									for (int i = 0; i < files.size(); i++) {
										printRAW (
											files[i],
											(getFilenamePath(files[i]) + subTokens(getFilenameFile(files[i])) + "_debug.txt"),
											lstfile,
											showNotePad
											);
										}
									}
								}
							else {
								if (dumppath != "") {
									for (int i = 0; i < files.size(); i++) {
										printRAW (
											files[i],
											(dumppath + getFilenameFile(files[i]) + "_debug.txt"),
											lstfile,
											showNotePad
											);
										}

									}
								else {
									for (int i = 0; i < files.size(); i++) {
										printRAW (
											files[i],
											(getFilenamePath(files[i]) + getFilenameFile(files[i]) + "_debug.txt"),
											lstfile,
											showNotePad
											);
										}

									}
								}
							}
						else {
							for (int i = 0; i < files.size(); i++) {
								printRAW (
									files[i],
									(getFilenamePath(files[i]) + getFilenameFile(files[i]) + "_debug.txt"),
									lstfile,
									false
									);
								}
							}

						}
					goto finishup;
					}
				else {
					goto read;
					}
read:
				if (files.size() > 0) {
                    readFieldsFromPR1File(pr1files, files, lstfile);
					if (files.size() > 1) {
						if (savepath == "") {
							savepath = inFolder + get_date() + ".csv";
							if (useTokens) {
								savepath = subTokens(savepath);
								}
							}
						printCSV (
							pr1files,
							lstfile,
							files.size(),
							savepath,
							showNotePad,
							files
							);
						}
					else {
						if (savepath == "") {
							savepath = getFilenamePath(files[0]) + getFilenameFile(files[0]) + ".csv";
							if (useTokens) {
								savepath = subTokens(savepath);
								}
							}
						printCSV (
							pr1files,
							lstfile,
							1,
							savepath,
							showNotePad,
							files
							);
						}
					for (int i = 0; i < files.size(); i++){
						delete[] pr1files[i];
						}
                    delete[] pr1files;
					}
				}
			}
		}
	else {
		// there are no arugments and no files supplied
		// try search root path of executable for compatible file types
		if (showMsgBox) {
			int btn = MessageBox( 0, msgstr.c_str(),  "About" ,MB_YESNO + MB_ICONQUESTION );
			}
		if ( btn == IDYES ) {
			read_directory(curFolder, "*.pr1", files);
            goto read;
			}
		}
finishup:
	cout << "Operation Completed\n" << endl;

	// Clean up

	delete[] lstfile;
	delete[] pr1files;
    return 0;
    }
