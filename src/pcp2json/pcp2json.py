#!/usr/bin/env pmpython
#
# Copyright (C) 2015-2017 Marko Myllynen <myllynen@redhat.com>
#
# This program is free software; you can redistribute it and/or modify it
# under the terms of the GNU General Public License as published by the
# Free Software Foundation; either version 2 of the License, or (at your
# option) any later version.
#
# This program is distributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
# or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
# for more details.

# [write_json] Copyright (C) 2014-2015 Red Hat, based on pcp2es by Frank Ch. Eigler

# pylint: disable=superfluous-parens
# pylint: disable=invalid-name, line-too-long, no-self-use
# pylint: disable=too-many-boolean-expressions, too-many-statements
# pylint: disable=too-many-instance-attributes, too-many-locals
# pylint: disable=too-many-branches, too-many-nested-blocks, too-many-arguments
# pylint: disable=bare-except, broad-except

""" PCP to JSON Bridge """

# Common imports
from collections import OrderedDict
import errno
import time
import sys

# Our imports
try:
    import json
except:
    import simplejson as json
import socket
import os

# PCP Python PMAPI
from pcp import pmapi, pmconfig
from cpmapi import PM_CONTEXT_ARCHIVE, PM_ERR_EOL, PM_IN_NULL, PM_DEBUG_APPL1
from cpmapi import PM_TIME_SEC

if sys.version_info[0] >= 3:
    long = int # pylint: disable=redefined-builtin

# Default config
DEFAULT_CONFIG = ["./pcp2json.conf", "$HOME/.pcp2json.conf", "$HOME/.pcp/pcp2json.conf", "$PCP_SYSCONF_DIR/pcp2json.conf"]

# Defaults
CONFVER = 1
INDENT = 2
TIMEFMT = "%Y-%m-%d %H:%M:%S"

class PCP2JSON(object):
    """ PCP to JSON """
    def __init__(self):
        """ Construct object, prepare for command line handling """
        self.context = None
        self.daemonize = 0
        self.pmconfig = pmconfig.pmConfig(self)
        self.opts = self.options()

        # Configuration directives
        self.keys = ('source', 'output', 'derived', 'header', 'globals',
                     'samples', 'interval', 'type', 'precision', 'daemonize',
                     'timefmt', 'extended', 'everything',
                     'count_scale', 'space_scale', 'time_scale', 'version',
                     'speclocal', 'instances', 'ignore_incompat', 'omit_flat')

        # The order of preference for options (as present):
        # 1 - command line options
        # 2 - options from configuration file(s)
        # 3 - built-in defaults defined below
        self.check = 0
        self.version = CONFVER
        self.source = "local:"
        self.output = None # For pmrep conf file compat only
        self.speclocal = None
        self.derived = None
        self.header = 1
        self.globals = 1
        self.samples = None # forever
        self.interval = pmapi.timeval(60)      # 60 sec
        self.opts.pmSetOptionInterval(str(60)) # 60 sec
        self.delay = 0
        self.type = 0
        self.ignore_incompat = 0
        self.instances = []
        self.omit_flat = 0
        self.precision = 3 # .3f
        self.timefmt = TIMEFMT
        self.interpol = 0
        self.count_scale = None
        self.space_scale = None
        self.time_scale = None

        # Not in pcp2json.conf, won't overwrite
        self.outfile = None

        self.extended = 0
        self.everything = 0

        # Internal
        self.runtime = -1

        self.data = None
        self.prev_ts = None
        self.writer = None

        # Performance metrics store
        # key - metric name
        # values - 0:label, 1:instance(s), 2:unit/scale, 3:type, 4:width, 5:pmfg item
        self.metrics = OrderedDict()
        self.pmfg = None
        self.pmfg_ts = None

        # Read configuration and prepare to connect
        self.config = self.pmconfig.set_config_file(DEFAULT_CONFIG)
        self.pmconfig.read_options()
        self.pmconfig.read_cmd_line()
        self.pmconfig.prepare_metrics()

    def options(self):
        """ Setup default command line argument option handling """
        opts = pmapi.pmOptions()
        opts.pmSetOptionCallback(self.option)
        opts.pmSetOverrideCallback(self.option_override)
        opts.pmSetShortOptions("a:h:LK:c:Ce:D:V?HGA:S:T:O:s:t:rIi:vP:q:b:y:F:f:Z:zxX")
        opts.pmSetShortUsage("[option...] metricspec [...]")

        opts.pmSetLongOptionHeader("General options")
        opts.pmSetLongOptionArchive()      # -a/--archive
        opts.pmSetLongOptionArchiveFolio() # --archive-folio
        opts.pmSetLongOptionContainer()    # --container
        opts.pmSetLongOptionHost()         # -h/--host
        opts.pmSetLongOptionLocalPMDA()    # -L/--local-PMDA
        opts.pmSetLongOptionSpecLocal()    # -K/--spec-local
        opts.pmSetLongOption("config", 1, "c", "FILE", "config file path")
        opts.pmSetLongOption("check", 0, "C", "", "check config and metrics and exit")
        opts.pmSetLongOption("output-file", 1, "F", "OUTFILE", "output file")
        opts.pmSetLongOption("derived", 1, "e", "FILE|DFNT", "derived metrics definitions")
        self.daemonize = opts.pmSetLongOption("daemonize", 0, "", "", "daemonize on startup") # > 1
        opts.pmSetLongOptionDebug()        # -D/--debug
        opts.pmSetLongOptionVersion()      # -V/--version
        opts.pmSetLongOptionHelp()         # -?/--help

        opts.pmSetLongOptionHeader("Reporting options")
        opts.pmSetLongOption("no-header", 0, "H", "", "omit headers")
        opts.pmSetLongOption("no-globals", 0, "G", "", "omit global metrics")
        opts.pmSetLongOptionAlign()        # -A/--align
        opts.pmSetLongOptionStart()        # -S/--start
        opts.pmSetLongOptionFinish()       # -T/--finish
        opts.pmSetLongOptionOrigin()       # -O/--origin
        opts.pmSetLongOptionSamples()      # -s/--samples
        opts.pmSetLongOptionInterval()     # -t/--interval
        opts.pmSetLongOptionTimeZone()     # -Z/--timezone
        opts.pmSetLongOptionHostZone()     # -z/--hostzone
        opts.pmSetLongOption("raw", 0, "r", "", "output raw counter values (no rate conversion)")
        opts.pmSetLongOption("ignore-incompat", 0, "I", "", "ignore incompatible instances (default: abort)")
        opts.pmSetLongOption("instances", 1, "i", "STR", "instances to report (default: all current)")
        opts.pmSetLongOption("omit-flat", 0, "v", "", "omit single-valued metrics with -i (default: include)")
        opts.pmSetLongOption("timestamp-format", 1, "f", "STR", "strftime string for timestamp format")
        opts.pmSetLongOption("precision", 1, "P", "N", "N digits after the decimal separator (default: 3)")
        opts.pmSetLongOption("count-scale", 1, "q", "SCALE", "default count unit")
        opts.pmSetLongOption("space-scale", 1, "b", "SCALE", "default space unit")
        opts.pmSetLongOption("time-scale", 1, "y", "SCALE", "default time unit")

        opts.pmSetLongOption("with-extended", 0, "x", "", "write extended information about metrics")
        opts.pmSetLongOption("with-everything", 0, "X", "", "write everything, incl. internal IDs")

        return opts

    def option_override(self, opt):
        """ Override standard PCP options """
        if opt == 'H' or opt == 'K':
            return 1
        return 0

    def option(self, opt, optarg, index):
        """ Perform setup for an individual command line option """
        if index == self.daemonize and opt == '':
            self.daemonize = 1
            return
        if opt == 'K':
            if not self.speclocal or not self.speclocal.startswith("K:"):
                self.speclocal = "K:" + optarg
            else:
                self.speclocal = self.speclocal + "|" + optarg
        elif opt == 'c':
            self.config = optarg
        elif opt == 'C':
            self.check = 1
        elif opt == 'F':
            if os.path.exists(optarg):
                sys.stderr.write("File %s already exists.\n" % optarg)
                sys.exit(1)
            self.outfile = optarg
        elif opt == 'e':
            self.derived = optarg
        elif opt == 'H':
            self.header = 0
        elif opt == 'G':
            self.globals = 0
        elif opt == 'r':
            self.type = 1
        elif opt == 'I':
            self.ignore_incompat = 1
        elif opt == 'i':
            self.instances = self.instances + self.pmconfig.parse_instances(optarg)
        elif opt == 'v':
            self.omit_flat = 1
        elif opt == 'P':
            try:
                self.precision = int(optarg)
            except:
                sys.stderr.write("Error while parsing options: Integer expected.\n")
                sys.exit(1)
        elif opt == 'f':
            self.timefmt = optarg
        elif opt == 'q':
            self.count_scale = optarg
        elif opt == 'b':
            self.space_scale = optarg
        elif opt == 'y':
            self.time_scale = optarg
        elif opt == 'x':
            self.extended = 1
        elif opt == 'X':
            self.everything = 1
        else:
            raise pmapi.pmUsageErr()

    def connect(self):
        """ Establish a PMAPI context """
        context, self.source = pmapi.pmContext.set_connect_options(self.opts, self.source, self.speclocal)

        self.pmfg = pmapi.fetchgroup(context, self.source)
        self.pmfg_ts = self.pmfg.extend_timestamp()
        self.context = self.pmfg.get_context()

        if pmapi.c_api.pmSetContextOptions(self.context.ctx, self.opts.mode, self.opts.delta):
            raise pmapi.pmUsageErr()

        self.pmconfig.validate_metrics()

    def validate_config(self):
        """ Validate configuration options """
        if self.version != CONFVER:
            sys.stderr.write("Incompatible configuration file version (read v%s, need v%d).\n" % (self.version, CONFVER))
            sys.exit(1)

        if self.everything:
            self.extended = 1

        self.pmconfig.finalize_options()

    def execute(self):
        """ Fetch and report """
        # Debug
        if self.context.pmDebug(PM_DEBUG_APPL1):
            sys.stdout.write("Known config file keywords: " + str(self.keys) + "\n")
            sys.stdout.write("Known metric spec keywords: " + str(self.pmconfig.metricspec) + "\n")

        # Set delay mode, interpolation
        if self.context.type != PM_CONTEXT_ARCHIVE:
            self.delay = 1
            self.interpol = 1

        # Common preparations
        self.context.prepare_execute(self.opts, False, self.interpol, self.interval)

        # Headers
        if self.header == 1:
            self.header = 0
            self.write_header()

        # Just checking
        if self.check == 1:
            return

        # Daemonize when requested
        if self.daemonize == 1:
            self.opts.daemonize()

        # Align poll interval to host clock
        if self.context.type != PM_CONTEXT_ARCHIVE and self.opts.pmGetOptionAlignment():
            align = float(self.opts.pmGetOptionAlignment()) - (time.time() % float(self.opts.pmGetOptionAlignment()))
            time.sleep(align)

        # Main loop
        while self.samples != 0:
            # Fetch values
            try:
                self.pmfg.fetch()
            except pmapi.pmErr as error:
                if error.args[0] == PM_ERR_EOL:
                    break
                raise error

            # Watch for endtime in uninterpolated mode
            if not self.interpol:
                if float(self.pmfg_ts().strftime('%s')) > float(self.opts.pmGetOptionFinish()):
                    break

            # Report and prepare for the next round
            self.report(self.pmfg_ts())
            if self.samples and self.samples > 0:
                self.samples -= 1
            if self.delay and self.interpol and self.samples != 0:
                self.pmconfig.pause()

        # Allow to flush buffered values / say goodbye
        self.report(None)

    def report(self, tstamp):
        """ Report the metric values """
        if tstamp != None:
            tstamp = tstamp.strftime(self.timefmt)

        self.write_json(tstamp)

    def write_header(self):
        """ Write info header """
        output = self.outfile if self.outfile else "stdout"
        if self.context.type == PM_CONTEXT_ARCHIVE:
            sys.stdout.write('{ "//": "Writing %d archived metrics to %s..." }\n{ "//": "(Ctrl-C to stop)" }\n' % (len(self.metrics), output))
            return

        sys.stdout.write('{ "//": "Waiting for %d metrics to be written to %s' % (len(self.metrics), output))
        if self.runtime != -1:
            sys.stdout.write(':" }\n{ "//": "%s samples(s) with %.1f sec interval ~ %d sec runtime." }\n' % (self.samples, float(self.interval), self.runtime))
        elif self.samples:
            duration = (self.samples - 1) * float(self.interval)
            sys.stdout.write(':" }\n{ "//": "%s samples(s) with %.1f sec interval ~ %d sec runtime." }\n' % (self.samples, float(self.interval), duration))
        else:
            sys.stdout.write('..." }\n{ "//": "(Ctrl-C to stop)" }\n')

    def write_json(self, timestamp):
        """ Write results in JSON format """
        if timestamp is None:
            # Silent goodbye, close in finalize()
            return

        ts = self.context.datetime_to_secs(self.pmfg_ts(), PM_TIME_SEC)

        if self.prev_ts is None:
            self.prev_ts = ts

        if not self.writer:
            if self.outfile is None:
                self.writer = sys.stdout
            else:
                self.writer = open(self.outfile, 'wt')

        # Assemble all metrics into a single document
        # Use @-prefixed keys for metadata not coming in from PCP metrics
        if not self.data:
            self.data = {'@pcp': {'@hosts': []}}
            host = self.context.pmGetContextHostName()
            self.data['@pcp']['@hosts'].append({'@host': host, '@source': self.source})
            self.data['@pcp']['@hosts'][0]['@timezone'] = self.context.posix_tz_to_utc_offset(self.context.get_current_tz(self.opts))
            self.data['@pcp']['@hosts'][0]['@metrics'] = []

        self.data['@pcp']['@hosts'][0]['@metrics'].append({'@timestamp': str(timestamp)})
        self.data['@pcp']['@hosts'][0]['@metrics'][-1]['@interval'] = str(int(ts - self.prev_ts + 0.5))
        self.prev_ts = ts

        insts_key = "@instances"
        inst_key = "@id"

        def get_type_string(desc):
            """ Get metric type as string """
            if desc.contents.type == pmapi.c_api.PM_TYPE_32:
                mtype = "32-bit signed"
            elif desc.contents.type == pmapi.c_api.PM_TYPE_U32:
                mtype = "32-bit unsigned"
            elif desc.contents.type == pmapi.c_api.PM_TYPE_64:
                mtype = "64-bit signed"
            elif desc.contents.type == pmapi.c_api.PM_TYPE_U64:
                mtype = "64-bit unsigned"
            elif desc.contents.type == pmapi.c_api.PM_TYPE_FLOAT:
                mtype = "32-bit float"
            elif desc.contents.type == pmapi.c_api.PM_TYPE_DOUBLE:
                mtype = "64-bit float"
            elif desc.contents.type == pmapi.c_api.PM_TYPE_STRING:
                mtype = "string"
            else:
                mtype = "unknown"
            return mtype

        def create_attrs(value, inst_id, inst_name, unit, pmid, desc):
            """ Create extra attribute string """
            data = {"value": value}
            if inst_name:
                data['name'] = inst_name
            if unit:
                data['@unit'] = unit
            if self.extended:
                data['@type'] = get_type_string(desc)
                data['@semantics'] = self.context.pmSemStr(desc.contents.sem)
            if self.everything:
                data['@pmid'] = pmid
                if desc.contents.indom != PM_IN_NULL:
                    data['@indom'] = desc.contents.indom
                if inst_id is not None:
                    data[inst_key] = str(inst_id)
            return data

        for i, metric in enumerate(self.metrics):
            try:
                # Install value into outgoing json/dict in key1{key2{key3=value}} style:
                # foo.bar.baz=value    =>  foo: { bar: { baz: value ...} }
                # foo.bar.noo[i]=value =>  foo: { bar: { noo: {@instances:[{i: value ...} ... ]}}}

                pmns_parts = metric.split(".")

                for inst, name, val in self.metrics[metric][5](): # pylint: disable=unused-variable
                    try:
                        value = val()
                        fmt = "." + str(self.precision) + "f"
                        value = format(value, fmt) if isinstance(value, float) else str(value)
                    except:
                        continue

                    pmns_leaf_dict = self.data['@pcp']['@hosts'][0]['@metrics'][-1]

                    # Find/create the parent dictionary into which to insert the final component
                    for pmns_part in pmns_parts[:-1]:
                        if pmns_part not in pmns_leaf_dict:
                            pmns_leaf_dict[pmns_part] = {}
                        pmns_leaf_dict = pmns_leaf_dict[pmns_part]
                    last_part = pmns_parts[-1]

                    if inst == PM_IN_NULL:
                        pmns_leaf_dict[last_part] = create_attrs(value, None, None, self.metrics[metric][2][0], self.pmconfig.pmids[i], self.pmconfig.descs[i])
                    else:
                        if last_part not in pmns_leaf_dict:
                            pmns_leaf_dict[last_part] = {insts_key: []}
                        insts = pmns_leaf_dict[last_part][insts_key]
                        insts.append(create_attrs(value, inst, name, self.metrics[metric][2][0], self.pmconfig.pmids[i], self.pmconfig.descs[i]))
            except:
                pass

    def finalize(self):
        """ Finalize and clean up """
        if self.writer:
            try:
                self.writer.write(json.dumps(self.data,
                                             indent=INDENT,
                                             sort_keys=True,
                                             ensure_ascii=False,
                                             separators=(',', ': ')))
                self.writer.write("\n")
                self.writer.flush()
            except socket.error as error:
                if error.errno != errno.EPIPE:
                    raise
            try:
                self.writer.close()
            except:
                pass
            self.writer = None
        return

if __name__ == '__main__':
    try:
        P = PCP2JSON()
        P.connect()
        P.validate_config()
        P.execute()
        P.finalize()

    except pmapi.pmErr as error:
        sys.stderr.write('%s: %s\n' % (error.progname(), error.message()))
        sys.exit(1)
    except pmapi.pmUsageErr as usage:
        usage.message()
        sys.exit(1)
    except IOError as error:
        if error.errno != errno.EPIPE:
            sys.stderr.write("%s\n" % str(error))
            sys.exit(1)
    except KeyboardInterrupt:
        sys.stdout.write("\n")
        P.finalize()
