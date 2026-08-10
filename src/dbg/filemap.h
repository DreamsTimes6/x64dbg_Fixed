#pragma once

#include <Windows.h>
#include <cstdint>

enum class FileMapAccess
{
    Read,
    Write,
    All,
};

inline DWORD FileMapDesiredFileAccess(FileMapAccess access)
{
    switch(access)
    {
    case FileMapAccess::Read:
        return GENERIC_READ;
    case FileMapAccess::Write:
        return GENERIC_WRITE;
    case FileMapAccess::All:
    default:
        return GENERIC_READ | GENERIC_WRITE | GENERIC_EXECUTE;
    }
}

inline DWORD FileMapProtect(FileMapAccess access)
{
    switch(access)
    {
    case FileMapAccess::Read:
        return PAGE_READONLY;
    case FileMapAccess::Write:
        return PAGE_READWRITE;
    case FileMapAccess::All:
    default:
        return PAGE_EXECUTE_READWRITE;
    }
}

inline DWORD FileMapViewAccess(FileMapAccess access)
{
    switch(access)
    {
    case FileMapAccess::Read:
        return FILE_MAP_READ;
    case FileMapAccess::Write:
        return FILE_MAP_READ | FILE_MAP_WRITE;
    case FileMapAccess::All:
    default:
        return FILE_MAP_ALL_ACCESS;
    }
}

inline bool MapExistingFileHandle(HANDLE hFile, FileMapAccess access, uint64_t & size, HANDLE & hMap, ULONG_PTR & mapView, uint64_t sizeModifier = 0)
{
    size = 0;
    hMap = nullptr;
    mapView = 0;

    if(hFile == nullptr || hFile == INVALID_HANDLE_VALUE)
        return false;

    LARGE_INTEGER fileSize = {};
    if(!GetFileSizeEx(hFile, &fileSize) || fileSize.QuadPart <= 0)
        return false;

    size = uint64_t(fileSize.QuadPart) + sizeModifier;
    hMap = CreateFileMappingW(hFile, nullptr, FileMapProtect(access), DWORD(size >> 32), DWORD(size & 0xFFFFFFFF), nullptr);
    if(!hMap)
        return false;

    auto view = MapViewOfFile(hMap, FileMapViewAccess(access), 0, 0, 0);
    if(!view)
    {
        CloseHandle(hMap);
        hMap = nullptr;
        size = 0;
        return false;
    }

    mapView = ULONG_PTR(view);
    return true;
}

inline bool MapFileW(const wchar_t* szFileName, FileMapAccess access, HANDLE & hFile, uint64_t & size, HANDLE & hMap, ULONG_PTR & mapView, uint64_t sizeModifier = 0)
{
    size = 0;
    hFile = INVALID_HANDLE_VALUE;
    hMap = nullptr;
    mapView = 0;

    hFile = CreateFileW(szFileName, FileMapDesiredFileAccess(access), FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if(hFile == INVALID_HANDLE_VALUE)
        return false;

    if(!MapExistingFileHandle(hFile, access, size, hMap, mapView, sizeModifier))
    {
        CloseHandle(hFile);
        hFile = INVALID_HANDLE_VALUE;
        return false;
    }

    return true;
}

inline bool UnmapFileView(HANDLE hFile, uint64_t size, HANDLE hMap, ULONG_PTR mapView, bool closeFileHandle = true, bool flush = false)
{
    bool success = true;

    if(flush && mapView)
        success = !!FlushViewOfFile((LPCVOID)mapView, 0) && success;

    if(mapView)
        success = !!UnmapViewOfFile((LPCVOID)mapView) && success;

    if(hMap)
        success = !!CloseHandle(hMap) && success;

    if(hFile != nullptr && hFile != INVALID_HANDLE_VALUE)
    {
        if(flush && size != 0)
        {
            LARGE_INTEGER distance;
            distance.QuadPart = size;
            success = !!SetFilePointerEx(hFile, distance, nullptr, FILE_BEGIN) && success;
            success = !!SetEndOfFile(hFile) && success;
        }

        if(flush)
            success = !!FlushFileBuffers(hFile) && success;

        if(closeFileHandle)
            success = !!CloseHandle(hFile) && success;
    }

    return success;
}

template<typename T>
struct FileMap
{
    bool Map(const wchar_t* szFileName, bool mapImage = false)
    {
        hFile = CreateFileW(szFileName, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if(hFile != INVALID_HANDLE_VALUE)
        {
            LARGE_INTEGER fileSizeLI;
            if(GetFileSizeEx(hFile, &fileSizeLI))
            {
                size = fileSizeLI.QuadPart;
                hMap = CreateFileMappingW(hFile, nullptr, PAGE_READONLY | (mapImage ? SEC_IMAGE : 0), 0, 0, nullptr);
                if(hMap)
                    data = (const T*)MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0);
            }
        }
        return data != nullptr;
    }

    uint64_t Size()
    {
        return size;
    }

    const T* Data()
    {
        return data;
    }

    void Unmap()
    {
        if(data)
            UnmapViewOfFile(data);
        if(hMap)
            CloseHandle(hMap);
        if(hFile != INVALID_HANDLE_VALUE)
            CloseHandle(hFile);

        hFile = INVALID_HANDLE_VALUE;
        hMap = nullptr;
        data = nullptr;
        size = 0;
    }

    ~FileMap()
    {
        Unmap();
    }

private:
    HANDLE hFile = INVALID_HANDLE_VALUE;
    HANDLE hMap = nullptr;
    const T* data = nullptr;
    uint64_t size = 0;
};

struct BufferedWriter
{
    explicit BufferedWriter(HANDLE hFile, size_t size = 65536)
        : hFile(hFile),
          mBuffer(new char[size]),
          mSize(size),
          mIndex(0)
    {
        memset(mBuffer, 0, size);
    }

    bool Write(const void* buffer, size_t size)
    {
        for(size_t i = 0; i < size; i++)
        {
            mBuffer[mIndex++] = ((const char*)buffer)[i];
            if(mIndex == mSize)
            {
                if(!flush())
                    return false;
                mIndex = 0;
            }
        }
        return true;
    }

    ~BufferedWriter()
    {
        flush();
        delete[] mBuffer;
        CloseHandle(hFile);
    }

    // Expose the pending-buffer flush so callers can detect write errors that
    // would otherwise only surface (and be swallowed) in the destructor.
    bool Flush()
    {
        return flush();
    }

private:
    HANDLE hFile;
    char* mBuffer;
    size_t mSize;
    size_t mIndex;

    bool flush()
    {
        if(!mIndex)
            return true;
        DWORD written = 0;
        auto toWrite = mIndex;
        auto result = WriteFile(hFile, mBuffer, DWORD(toWrite), &written, nullptr);
        mIndex = 0;
        return !!result && written == toWrite;
    }
};