// Small xboxkrnl Rtl* helpers.

#include "kernel.h"
#include "../platform/platform.h"

#include <chrono>

namespace
{
    // NT times (100 ns since 1601-01-01 UTC) <-> calendar fields, as FileTimeToSystemTime and
    // SystemTimeToFileTime do (proleptic Gregorian, years 1601-30827).
    using Ticks = std::chrono::duration<int64_t, std::ratio<1, 10000000>>;
    constexpr std::chrono::sys_days NT_EPOCH = std::chrono::sys_days(std::chrono::year(1601) / 1 / 1);

    struct TimeFields
    {
        uint16_t year, month, day, hour, minute, second, milliseconds, weekday;
    };

    TimeFields FieldsFromTime(uint64_t time)
    {
        using namespace std::chrono;
        Ticks ticks(int64_t(time & 0x7FFFFFFFFFFFFFFFull));
        auto dayCount = floor<days>(ticks);
        year_month_day date(NT_EPOCH + dayCount);
        auto rest = ticks - dayCount;
        hh_mm_ss<Ticks> clock(rest);
        TimeFields f;
        f.year = uint16_t(int(date.year()));
        f.month = uint16_t(unsigned(date.month()));
        f.day = uint16_t(unsigned(date.day()));
        f.hour = uint16_t(clock.hours().count());
        f.minute = uint16_t(clock.minutes().count());
        f.second = uint16_t(clock.seconds().count());
        f.milliseconds = uint16_t(duration_cast<milliseconds>(clock.subseconds()).count());
        f.weekday = uint16_t(weekday(sys_days(NT_EPOCH + dayCount)).c_encoding());
        return f;
    }

    bool TimeFromFields(const TimeFields& f, uint64_t& time)
    {
        using namespace std::chrono;
        year_month_day date{ year(f.year), month(f.month), day(f.day) };
        if (f.year < 1601 || f.year > 30827 || !date.ok() || f.hour > 23 || f.minute > 59 || f.second > 59 || f.milliseconds > 999)
            return false;
        Ticks ticks = sys_days(date) - NT_EPOCH;
        ticks += hours(f.hour) + minutes(f.minute) + seconds(f.second) + milliseconds(f.milliseconds);
        time = uint64_t(ticks.count());
        return true;
    }
}

// void RtlFillMemoryUlong(void* Destination, u32 Length, u32 Pattern): Length in bytes, filled
// with the big-endian dword Pattern.
PPC_FUNC(__imp__RtlFillMemoryUlong)
{
    SetLastImportCalled("RtlFillMemoryUlong");

    uint32_t destination = ctx.r3.u32;
    uint32_t count = ctx.r4.u32 / 4;
    uint32_t pattern = ctx.r5.u32;
    for (uint32_t i = 0; i < count; i++)
        PPC_STORE_U32(destination + i * 4, pattern);
}

// u32 RtlCompareMemoryUlong(void* Source, u32 Length, u32 Pattern): number of leading bytes
// (in whole dwords) equal to the big-endian dword Pattern.
PPC_FUNC(__imp__RtlCompareMemoryUlong)
{
    SetLastImportCalled("RtlCompareMemoryUlong");

    uint32_t source = ctx.r3.u32;
    uint32_t count = ctx.r4.u32 / 4;
    uint32_t pattern = ctx.r5.u32;
    uint32_t matched = 0;
    while (matched < count && PPC_LOAD_U32(source + matched * 4) == pattern)
        matched++;
    ctx.r3.u64 = matched * 4;
}

// u32 RtlNtStatusToDosError(NTSTATUS Status): the Win32 error code the XAPI stores for
// GetLastError (CreateFile & co. call it on failure). The stub returned 0, so a missing file read
// as "no error": Radical's file layer (sub_82296A90) then reported its "no error" code to the
// game's drive error callback, which answers "retry" and reopened the file forever.
// Mappings are Windows' for the statuses the kernel can return.
PPC_FUNC(__imp__RtlNtStatusToDosError)
{
    KERNEL_IMPORT_ENTER(RtlNtStatusToDosError);

    struct Mapping { uint32_t status, error; };
    static constexpr Mapping MAPPINGS[] = {
        { 0x00000000, 0 },     // STATUS_SUCCESS
        { 0x00000102, 258 },   // STATUS_TIMEOUT -> WAIT_TIMEOUT
        { 0x00000103, 997 },   // STATUS_PENDING -> ERROR_IO_PENDING
        { 0x80000005, 234 },   // STATUS_BUFFER_OVERFLOW -> ERROR_MORE_DATA
        { 0x80000006, 18 },    // STATUS_NO_MORE_FILES
        { 0xC0000001, 31 },    // STATUS_UNSUCCESSFUL -> ERROR_GEN_FAILURE
        { 0xC0000002, 1 },     // STATUS_NOT_IMPLEMENTED -> ERROR_INVALID_FUNCTION
        { 0xC0000008, 6 },     // STATUS_INVALID_HANDLE
        { 0xC000000D, 87 },    // STATUS_INVALID_PARAMETER
        { 0xC000000F, 2 },     // STATUS_NO_SUCH_FILE -> ERROR_FILE_NOT_FOUND
        { 0xC0000010, 1 },     // STATUS_INVALID_DEVICE_REQUEST -> ERROR_INVALID_FUNCTION
        { 0xC0000011, 38 },    // STATUS_END_OF_FILE -> ERROR_HANDLE_EOF
        { 0xC0000013, 21 },    // STATUS_NO_MEDIA_IN_DEVICE -> ERROR_NOT_READY
        { 0xC0000014, 1785 },  // STATUS_UNRECOGNIZED_MEDIA
        { 0xC0000017, 8 },     // STATUS_NO_MEMORY -> ERROR_NOT_ENOUGH_MEMORY
        { 0xC0000022, 5 },     // STATUS_ACCESS_DENIED
        { 0xC0000023, 122 },   // STATUS_BUFFER_TOO_SMALL -> ERROR_INSUFFICIENT_BUFFER
        { 0xC0000024, 6 },     // STATUS_OBJECT_TYPE_MISMATCH -> ERROR_INVALID_HANDLE
        { 0xC0000033, 123 },   // STATUS_OBJECT_NAME_INVALID -> ERROR_INVALID_NAME
        { 0xC0000034, 2 },     // STATUS_OBJECT_NAME_NOT_FOUND -> ERROR_FILE_NOT_FOUND
        { 0xC0000035, 183 },   // STATUS_OBJECT_NAME_COLLISION -> ERROR_ALREADY_EXISTS
        { 0xC0000039, 161 },   // STATUS_OBJECT_PATH_INVALID -> ERROR_BAD_PATHNAME
        { 0xC000003A, 3 },     // STATUS_OBJECT_PATH_NOT_FOUND -> ERROR_PATH_NOT_FOUND
        { 0xC000003B, 161 },   // STATUS_OBJECT_PATH_SYNTAX_BAD -> ERROR_BAD_PATHNAME
        { 0xC000003E, 23 },    // STATUS_DATA_ERROR -> ERROR_CRC
        { 0xC000003F, 23 },    // STATUS_CRC_ERROR -> ERROR_CRC
        { 0xC0000043, 32 },    // STATUS_SHARING_VIOLATION
        { 0xC0000056, 5 },     // STATUS_DELETE_PENDING -> ERROR_ACCESS_DENIED
        { 0xC000007F, 112 },   // STATUS_DISK_FULL
        { 0xC000009A, 1450 },  // STATUS_INSUFFICIENT_RESOURCES -> ERROR_NO_SYSTEM_RESOURCES
        { 0xC000009C, 23 },    // STATUS_DEVICE_DATA_ERROR -> ERROR_CRC
        { 0xC000009D, 21 },    // STATUS_DEVICE_NOT_CONNECTED -> ERROR_NOT_READY
        { 0xC00000A2, 19 },    // STATUS_MEDIA_WRITE_PROTECTED -> ERROR_WRITE_PROTECT
        { 0xC00000A3, 21 },    // STATUS_DEVICE_NOT_READY -> ERROR_NOT_READY
        { 0xC00000BA, 5 },     // STATUS_FILE_IS_A_DIRECTORY -> ERROR_ACCESS_DENIED
        { 0xC00000BB, 50 },    // STATUS_NOT_SUPPORTED
        { 0xC0000101, 145 },   // STATUS_DIRECTORY_NOT_EMPTY -> ERROR_DIR_NOT_EMPTY
        { 0xC0000102, 1392 },  // STATUS_FILE_CORRUPT_ERROR
        { 0xC0000103, 267 },   // STATUS_NOT_A_DIRECTORY -> ERROR_DIRECTORY
        { 0xC0000120, 995 },   // STATUS_CANCELLED -> ERROR_OPERATION_ABORTED
        { 0xC0000225, 1168 },  // STATUS_NOT_FOUND
    };

    uint32_t status = ctx.r3.u32;
    uint32_t error = 317; // ERROR_MR_MID_NOT_FOUND, Windows' answer for unmapped statuses
    if ((status & 0x0FFF0000) == 0x00070000)
        error = status & 0xFFFF; // HRESULT_FROM_WIN32 / FACILITY_WIN32 status
    else
    {
        for (const Mapping& mapping : MAPPINGS)
        {
            if (mapping.status == status)
            {
                error = mapping.error;
                break;
            }
        }
    }
    KERNEL_LOG("[kernel] RtlNtStatusToDosError(0x%08X) lr=0x%08X -> %u", status, GuestLR(ctx), error);
    ctx.r3.u64 = error;
}

// void KeQuerySystemTime(u64* CurrentTime): the host's UTC time as an NT FILETIME (100 ns since
// 1601-01-01), like the console's clock. The XAPI's GetSystemTime/GetLocalTime build on it (save
// game dates; the stub left them as garbage, e.g. "Last Save: 255.2.35").
PPC_FUNC(__imp__KeQuerySystemTime)
{
    KERNEL_IMPORT_ENTER(KeQuerySystemTime);
    uint64_t time = plat::SystemTimeFileTime();
    if (ctx.r3.u32)
        PPC_STORE_U64(ctx.r3.u32, time);
    KERNEL_LOG("[kernel] KeQuerySystemTime(0x%08X) lr=0x%08X -> 0x%016llX", ctx.r3.u32, GuestLR(ctx), (unsigned long long)time);
}

// TIME_FIELDS: s16 Year, Month, Day, Hour, Minute, Second, Milliseconds, Weekday (0 = Sunday).

// void RtlTimeToTimeFields(u64* Time, TIME_FIELDS* TimeFields)
PPC_FUNC(__imp__RtlTimeToTimeFields)
{
    KERNEL_IMPORT_ENTER(RtlTimeToTimeFields);
    uint64_t time = PPC_LOAD_U64(ctx.r3.u32);
    TimeFields fields = FieldsFromTime(time);
    uint32_t out = ctx.r4.u32;
    PPC_STORE_U16(out + 0x0, fields.year);
    PPC_STORE_U16(out + 0x2, fields.month);
    PPC_STORE_U16(out + 0x4, fields.day);
    PPC_STORE_U16(out + 0x6, fields.hour);
    PPC_STORE_U16(out + 0x8, fields.minute);
    PPC_STORE_U16(out + 0xA, fields.second);
    PPC_STORE_U16(out + 0xC, fields.milliseconds);
    PPC_STORE_U16(out + 0xE, fields.weekday);
    KERNEL_LOG("[kernel] RtlTimeToTimeFields(0x%016llX) lr=0x%08X -> %04u-%02u-%02u %02u:%02u:%02u", (unsigned long long)time,
               GuestLR(ctx), fields.year, fields.month, fields.day, fields.hour, fields.minute, fields.second);
}

// BOOLEAN RtlTimeFieldsToTime(TIME_FIELDS* TimeFields, u64* Time): FALSE for an invalid date.
PPC_FUNC(__imp__RtlTimeFieldsToTime)
{
    KERNEL_IMPORT_ENTER(RtlTimeFieldsToTime);
    uint32_t in = ctx.r3.u32;
    TimeFields fields{};
    fields.year = PPC_LOAD_U16(in + 0x0);
    fields.month = PPC_LOAD_U16(in + 0x2);
    fields.day = PPC_LOAD_U16(in + 0x4);
    fields.hour = PPC_LOAD_U16(in + 0x6);
    fields.minute = PPC_LOAD_U16(in + 0x8);
    fields.second = PPC_LOAD_U16(in + 0xA);
    fields.milliseconds = PPC_LOAD_U16(in + 0xC);
    uint64_t time = 0;
    bool ok = TimeFromFields(fields, time);
    if (ok && ctx.r4.u32)
        PPC_STORE_U64(ctx.r4.u32, time);
    KERNEL_LOG("[kernel] RtlTimeFieldsToTime(%04u-%02u-%02u) lr=0x%08X -> %u", fields.year, fields.month, fields.day, GuestLR(ctx), ok);
    ctx.r3.u64 = ok;
}
