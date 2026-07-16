DEBUG = False


def debugPrint(*args, **kwargs):
    if DEBUG:
        print(*args, **kwargs)
