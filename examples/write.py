#!/usr/bin/python
# -*- mode: python; coding: utf-8 -*-

# Copyright (C) 2014, Oscar Acena <oscar.acena@uclm.es>
# This software is under the terms of GPLv3 or later.

import sys
from gattlib import GATTRequester


class Reader(object):
    def __init__(self, address):
        self.requester = GATTRequester(address)
        self.connect()
        self.send_data()

    def connect(self):
        print "Connecting...",
        sys.stdout.flush()

        self.requester.wait_connection()
        print "OK!"

    def send_data(self):
        self.requester.write_by_handle(0x2e, str(bytearray([2])))


if __name__ == '__main__':
    if len(sys.argv) < 2:
        print "Usage: {} <addr>".format(sys.argv[0])
        sys.exit(1)

    Reader(sys.argv[1])
    print "Done."
