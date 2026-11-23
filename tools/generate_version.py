"""Generate every display and binary version from the build's version tuple."""
from datetime import datetime, date
from pathlib import Path
import sys

destination, major, minor, stamp, build, mode = sys.argv[1:]
major, minor, build = int(major), int(minor), int(build)
build_date = datetime.strptime(stamp, "%Y%m%d").date()
# Windows fixed versions have four 16-bit words. The full date remains in the
# display strings; an epoch-day field keeps the numeric version sortable.
epoch_day = (build_date - date(2020, 1, 1)).days
if not all(0 <= value <= 65535 for value in (major, minor, epoch_day, build)):
    raise SystemExit("Version exceeds the Windows fixed-version field range")
version = f"{major}.{minor}.{stamp}.{build}"
Path(destination).write_text(f'''#ifndef SIMP_VERSION_GENERATED_H
#define SIMP_VERSION_GENERATED_H
#define APP_VERSION_STRING "{version}"
#define APP_VERSION_WSTRING L"{version}"
#define APP_VERSION_MAJOR {major}
#define APP_VERSION_MINOR {minor}
#define APP_VERSION_DATE {stamp}
#define APP_VERSION_BUILD {build}
#define APP_VERSION_RESOURCE_TUPLE {major},{minor},{epoch_day},{build}
#define APP_VERSION_FILE_FLAGS {1 if mode == "debug" else 0}
#endif
''')
