import sys

# Windows consoles are often on a codepage (e.g. cp1252) that can't encode
# arbitrary Unicode -- a song/artist title containing an em-dash, arrow, or
# emoji would otherwise crash print() with a UnicodeEncodeError and abort
# whichever loop iteration was debug-printing it.
try:
    sys.stdout.reconfigure(errors="replace")
except (AttributeError, ValueError):
    pass

DEBUG = False


def debugPrint(*args, **kwargs):
    if DEBUG:
        print(*args, **kwargs)
