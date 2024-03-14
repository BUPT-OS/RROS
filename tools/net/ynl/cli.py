#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause

import argparse
import json
import pprint
import time

from lib import YnlFamily, Netlink


def main():
    parser = argparse.ArgumentParser(description='YNL CLI sample')
    parser.add_argument('--spec', dest='spec', type=str, required=True)
    parser.add_argument('--schema', dest='schema', type=str)
    parser.add_argument('--no-schema', action='store_true')
    parser.add_argument('--json', dest='json_text', type=str)
    parser.add_argument('--do', dest='do', type=str)
    parser.add_argument('--dump', dest='dump', type=str)
    parser.add_argument('--sleep', dest='sleep', type=int)
    parser.add_argument('--subscribe', dest='ntf', type=str)
    parser.add_argument('--replace', dest='flags', action='append_const',
                        const=Netlink.NLM_F_REPLACE)
    parser.add_argument('--excl', dest='flags', action='append_const',
                        const=Netlink.NLM_F_EXCL)
    parser.add_argument('--create', dest='flags', action='append_const',
                        const=Netlink.NLM_F_CREATE)
    parser.add_argument('--append', dest='flags', action='append_const',
                        const=Netlink.NLM_F_APPEND)
    args = parser.parse_args()

    if args.no_schema:
        args.schema = ''

    attrs = {}
    if args.json_text:
        attrs = json.loads(args.json_text)

    ynl = YnlFamily(args.spec, args.schema)

    if args.ntf:
        ynl.ntf_subscribe(args.ntf)

    if args.sleep:
        time.sleep(args.sleep)

    if args.do:
        reply = ynl.do(args.do, attrs, args.flags)
        pprint.PrettyPrinter().pprint(reply)
    if args.dump:
        reply = ynl.dump(args.dump, attrs)
        pprint.PrettyPrinter().pprint(reply)

    if args.ntf:
        ynl.check_ntf()
        pprint.PrettyPrinter().pprint(ynl.async_msg_queue)


if __name__ == "__main__":
    main()
