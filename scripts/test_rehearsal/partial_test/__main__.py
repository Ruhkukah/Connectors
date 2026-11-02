"""Deliberately has no activation path, configuration parser or network code."""
from . import REVIEW_ONLY


def main():
    if REVIEW_ONLY:
        raise SystemExit('REVIEW_ONLY: no deployment, native process or session may be armed')
    raise SystemExit('No native launcher is implemented in this review package')


if __name__ == '__main__':
    main()
