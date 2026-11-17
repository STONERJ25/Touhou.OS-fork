"""The datetime, calendar, time and zoneinfo modules with fixed values: arithmetic and comparison of dates and times, formatting and parsing, time zones (UTC and fixed offsets), the C
library's conversions (gmtime, localtime and mktime with TZ=UTC, strftime, strptime), the calendar and timestamps. Nothing here reads the clock."""
import calendar
import datetime as dt
import time
import zoneinfo


def show(label, value=""):
    print(f"{label}: {value}")


print("--- dates and times")
d = dt.date(2026, 11, 17)
show("date", (d, d.year, d.weekday(), d.isoweekday(), d.isocalendar(), d.toordinal(), d.timetuple().tm_yday, d.isoformat(), d.ctime()))
show("arithmetic", (d + dt.timedelta(days=45), d - dt.date(2000, 2, 29), d.replace(month=2, day=28), dt.date(2024, 2, 29) + dt.timedelta(days=365), d - dt.timedelta(weeks=60)))
show("min/max", (dt.date.min, dt.date.max, dt.date.fromordinal(1), dt.date.fromisoformat("2024-02-29"), dt.date.fromisocalendar(2026, 1, 1)))
t = dt.datetime(2026, 11, 17, 13, 45, 59, 123456)
show("datetime", (t, t.isoformat(), t.isoformat(sep=" ", timespec="seconds"), t.time(), t.date(), t.microsecond, t.replace(hour=0, microsecond=0), str(t)))
show("timedelta", (dt.timedelta(days=1, hours=25, minutes=61, seconds=61.5), dt.timedelta(microseconds=-1), dt.timedelta(days=-2, seconds=3), dt.timedelta(hours=36).total_seconds(), abs(dt.timedelta(days=-1)), dt.timedelta(days=7) / 3, dt.timedelta(days=7) // dt.timedelta(hours=5), dt.timedelta(minutes=90) * 2.5))
show("compare", (dt.datetime(2026, 1, 1) < dt.datetime(2026, 1, 2), dt.date(2026, 1, 1) == dt.date(2026, 1, 1), max(dt.date(2020, 1, 1), dt.date(2021, 1, 1)), sorted([dt.time(5), dt.time(1, 30), dt.time(23)])))
show("strftime", (t.strftime("%Y-%m-%d %H:%M:%S.%f"), t.strftime("%A %d %B %Y, %I:%M %p"), t.strftime("%a %b %e %j %U %W %w %y %%"), t.strftime("%H%M%S"), t.strftime("%G-W%V-%u")))
show("strptime", (dt.datetime.strptime("2026-11-17 08:05:09", "%Y-%m-%d %H:%M:%S"), dt.datetime.strptime("Tue 17 Nov 2026", "%a %d %b %Y"), dt.datetime.strptime("17/11/26 3:07 PM", "%d/%m/%y %I:%M %p")))
show("fromisoformat", (dt.datetime.fromisoformat("2026-11-17T13:45:59"), dt.datetime.fromisoformat("2026-11-17T13:45:59+09:00"), dt.datetime.fromisoformat("2026-11-17 13:45:59.5Z"), dt.time.fromisoformat("08:30")))
for bad in ("2026-13-01", "2026-02-30", "garbage"):
    try:
        dt.date.fromisoformat(bad)
    except ValueError as e:
        show(f"bad date {bad!r}", e)
try:
    dt.datetime.strptime("2026-11-17", "%d/%m/%Y")
except ValueError as e:
    show("strptime mismatch", e)
try:
    dt.date(2026, 2, 29)
except ValueError as e:
    show("no 29 February", e)
try:
    dt.date(9999, 12, 31) + dt.timedelta(days=1)
except OverflowError as e:
    show("overflow", e)

print("--- time zones")
utc = dt.timezone.utc
tokyo = dt.timezone(dt.timedelta(hours=9), "JST")
nyc = dt.timezone(-dt.timedelta(hours=5))
a = dt.datetime(2026, 11, 17, 12, 0, tzinfo=utc)
show("aware", (a, a.astimezone(tokyo), a.astimezone(nyc), a.astimezone(tokyo).tzname(), a.astimezone(nyc).tzname(), a.utcoffset(), a.astimezone(tokyo).isoformat()))
show("difference", (a - a.astimezone(tokyo).replace(hour=0), a == a.astimezone(tokyo), a.timestamp(), dt.datetime(2026, 11, 17, 12, 0).timestamp()))
try:
    a < dt.datetime(2026, 1, 1)
except TypeError as e:
    show("naive and aware", e)
show("timestamps", (dt.datetime.fromtimestamp(0, utc), dt.datetime.fromtimestamp(1_700_000_000, tokyo), dt.datetime.fromtimestamp(86400 * 365, utc), dt.datetime.fromtimestamp(1.5, utc)))
z = zoneinfo.ZoneInfo("UTC")
show("zoneinfo UTC", (dt.datetime(2026, 11, 17, 12, 0, tzinfo=z), z.key, dt.datetime(2026, 11, 17, tzinfo=z).utcoffset()))
tokyo_z = zoneinfo.ZoneInfo("Asia/Tokyo")
ny = zoneinfo.ZoneInfo("America/New_York")
show("zones of the database", (a.astimezone(tokyo_z), a.astimezone(ny), a.astimezone(ny).tzname(), tokyo_z.key, dt.datetime(2026, 1, 15, 12, tzinfo=zoneinfo.ZoneInfo("Australia/Sydney")).utcoffset()))
before = dt.datetime(2026, 3, 8, 1, 59, tzinfo=ny)
show("spring forward", (before.utcoffset(), (before.astimezone(utc) + dt.timedelta(minutes=1)).astimezone(ny), dt.datetime(2026, 3, 8, 3, 0, tzinfo=ny).utcoffset()))
fold0, fold1 = dt.datetime(2026, 11, 1, 1, 30, tzinfo=ny, fold=0), dt.datetime(2026, 11, 1, 1, 30, tzinfo=ny, fold=1)
show("fall back", (fold0.utcoffset(), fold1.utcoffset(), fold0.tzname(), fold1.tzname()))
show("available zones", ("Europe/Paris" in zoneinfo.available_timezones(), "Asia/Tokyo" in zoneinfo.available_timezones(), len(zoneinfo.available_timezones()) > 300))
try:
    zoneinfo.ZoneInfo("Mars/Olympus")
except zoneinfo.ZoneInfoNotFoundError as e:
    show("unknown zone", type(e).__name__)
show("TZ=UTC", (time.tzname, time.timezone, time.daylight, dt.datetime.fromtimestamp(1_700_000_000), dt.datetime.fromtimestamp(1_700_000_000).astimezone().tzname()))
import os  # noqa: E402

for tz in ("America/New_York", "Asia/Tokyo", "Europe/London", "UTC"):         # the C library reads the zone's file: the local time and the abbreviations follow TZ
    os.environ["TZ"] = tz
    time.tzset()
    summer, winter = time.localtime(1_720_000_000), time.localtime(1_700_000_000)
    show(f"TZ={tz}", (time.tzname, time.timezone, time.daylight, time.strftime("%Y-%m-%d %H:%M:%S %Z %z", summer), time.strftime("%Y-%m-%d %H:%M:%S %Z %z", winter),
                       time.mktime((2026, 7, 1, 12, 0, 0, 0, 0, -1)), dt.datetime.fromtimestamp(1_720_000_000).isoformat()))
os.environ["TZ"] = "UTC"
time.tzset()

print("--- the time module")
show("gmtime", (time.gmtime(0), time.gmtime(1_700_000_000).tm_yday, time.gmtime(-1).tm_year, tuple(time.gmtime(2 ** 31))[:6]))
show("localtime is UTC", (time.localtime(1_700_000_000) == time.gmtime(1_700_000_000), time.localtime(1_700_000_000).tm_zone, time.localtime(1_700_000_000).tm_gmtoff))
show("mktime", (time.mktime((2026, 11, 17, 13, 45, 59, 0, 0, 0)), time.mktime(time.localtime(1_700_000_000)), time.mktime((2026, 14, 35, 25, 61, 61, 0, 0, 0))))
show("strftime", (time.strftime("%Y-%m-%d %H:%M:%S %Z %z", time.gmtime(1_700_000_000)), time.strftime("%c", time.gmtime(1_700_000_000)), time.strftime("%x %X", time.gmtime(1_700_000_000)), time.strftime("%j %U", time.gmtime(86400 * 100))))
show("asctime/ctime", (time.asctime(time.gmtime(1_700_000_000)), time.ctime(1_700_000_000)))
show("strptime", (time.strptime("2026-11-17 08:05:09", "%Y-%m-%d %H:%M:%S"), time.strptime("Nov 17 2026", "%b %d %Y").tm_wday))
show("struct_time", (time.gmtime(1_700_000_000).tm_mon, time.gmtime(1_700_000_000)[:3], len(time.gmtime(0))))
show("clocks exist", [time.get_clock_info(n).implementation for n in ("monotonic", "perf_counter", "time", "process_time", "thread_time")])
show("clock ids", (time.CLOCK_REALTIME, time.CLOCK_MONOTONIC, time.CLOCK_PROCESS_CPUTIME_ID, time.CLOCK_THREAD_CPUTIME_ID, time.CLOCK_BOOTTIME))
show("resolution is positive", all(time.clock_getres(c) > 0 for c in (time.CLOCK_REALTIME, time.CLOCK_MONOTONIC)))

print("--- calendar")
show("monthrange", (calendar.monthrange(2026, 11), calendar.monthrange(2024, 2), calendar.isleap(1900), calendar.isleap(2000), calendar.leapdays(2000, 2030)))
print(calendar.month(2026, 11))
show("weekday", (calendar.weekday(2026, 11, 17), calendar.day_name[calendar.weekday(2026, 11, 17)], calendar.month_abbr[11], calendar.timegm((2026, 11, 17, 13, 45, 59))))
show("monthcalendar", calendar.monthcalendar(2026, 2))
cal = calendar.TextCalendar(calendar.SUNDAY)
show("TextCalendar", cal.formatmonth(2026, 2, w=2).splitlines()[:3])
show("a whole year", len(calendar.calendar(2026).splitlines()))
show("iterweekdays", list(calendar.Calendar(firstweekday=0).iterweekdays()))
print("--- done")
