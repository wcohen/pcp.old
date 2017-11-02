#!/usr/bin/env pmpython
#
# Copyright (C) 2014-2017 Red Hat
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

# pylint: disable=superfluous-parens
# pylint: disable=invalid-name, line-too-long, no-self-use
# pylint: disable=too-many-boolean-expressions, too-many-statements
# pylint: disable=too-many-instance-attributes, too-many-locals
# pylint: disable=too-many-branches, too-many-nested-blocks
# pylint: disable=bare-except, broad-except

""" PCP to InfluxDB Bridge """

# Common imports
from collections import OrderedDict
import errno
import time
import sys

# Our imports
import re
import requests

# PCP Python PMAPI
from pcp import pmapi, pmconfig
from cpmapi import PM_CONTEXT_ARCHIVE, PM_ERR_EOL, PM_IN_NULL, PM_DEBUG_APPL1
from cpmapi import PM_TIME_NSEC

if sys.version_info[0] >= 3:
    long = int # pylint: disable=redefined-builtin

# Default config
DEFAULT_CONFIG = ["./pcp2influxdb.conf", "$HOME/.pcp2influxdb.conf", "$HOME/.pcp/pcp2influxdb.conf", "$PCP_SYSCONF_DIR/pcp2influxdb.conf"]

# Defaults
CONFVER = 1
SERVER = "http://127.0.0.1:8086"
DB = "pcp"

class Metric(object):
    """ A wrapper around metrics, due to InfluxDB's non-hierarchical way of
    organizing metrics

    For example, take disk.partitions.read, which (on a test system) has 4
    instances (sda1, sda2, sda3, and sr0).

    For InfluxDB, the proper solution is to submit a "measurement" with 4
    "fields". The request body could look like:

        disk_partitions_read,host=myhost.example.com sda1=5,sda2=4,sda3=10,sr0=0 1147483647000000000

    If there is only a single value, like for proc.nprocs, then the field key
    will be "value". For example:

        proc_nprocs,host=myhost.example.com value=200 1147483647000000000

    This class deals with this format.
    """

    def __init__(self, name):
        self.name = self.sanitize_name(name)
        self.fields = dict()
        self.tags = None
        self.ts = None

    def add_field(self, key="value", value=None):
        """ Add field """
        if value:
            self.fields[key] = value

    def set_tag_string(self, tag_str):
        """ Set tag string """
        self.tags = tag_str

    def sanitize_name(self, name):
        """ Sanitize name """
        tmp = name

        for c in ['.', '-']:
            tmp = tmp.replace(c, '_')

        while '__' in tmp:
            tmp = tmp.replace('__', '_')

        return tmp

    def set_timestamp(self, ts):
        """ Set timestamp """
        self.ts = ts

    def __str__(self):
        tmp = self.name
        if self.tags:
            tmp += ',' + self.tags

        tmp += ' '

        fields = []
        for k in self.fields:
            fields.append(k + '=' + str(self.fields[k]))

        tmp += ','.join(fields)
        tmp += ' '

        tmp += str(self.ts)

        return tmp

class WriteBody(object): # pylint: disable=too-few-public-methods
    """ Create a request to POST to /write on an InfluxDB server

    name will be used for the measurement name after it has been
    transformed to be allowable. Characters like '-' and '.' will be replaced
    with '_', and multiple underscores in a row will be replaced with a single
    underscore.

    value will be put into the measurement with a field key of 'value'.
    It should be a numeric type, but it will _not_ be checked, just cast
    directly to a string.

    timestamp should be an integer that is unix time from epoch in seconds.

    tags should be a dictionary of tags to add, with keys being tag
    keys and values being tag values.
    """

    def __init__(self):
        self.metrics = []

    def add(self, metric):
        """ Add metric """
        if metric.fields:
            self.metrics.append(metric)

    def __str__(self):
        if self.metrics:
            return "\n".join(str(s) for s in self.metrics)

        raise ValueError("Invalid request - no metrics")

class PCP2InfluxDB(object):
    """ PCP to InfluxDB """
    def __init__(self):
        """ Construct object, prepare for command line handling """
        self.context = None
        self.daemonize = 0
        self.pmconfig = pmconfig.pmConfig(self)
        self.opts = self.options()

        # Configuration directives
        self.keys = ('source', 'output', 'derived', 'header', 'globals',
                     'samples', 'interval', 'type', 'precision', 'daemonize',
                     'influx_server', 'influx_db',
                     'influx_user', 'influx_pass', 'influx_tags',
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
        self.timefmt = "%H:%M:%S" # For compat only
        self.interpol = 0
        self.count_scale = None
        self.space_scale = None
        self.time_scale = None

        self.influx_server = SERVER
        self.influx_db = DB
        self.influx_user = None
        self.influx_pass = None
        self.influx_tags = ""

        # Internal
        self.runtime = -1

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
        opts.pmSetShortOptions("a:h:LK:c:Ce:D:V?HGA:S:T:O:s:t:rIi:vP:q:b:y:g:x:U:E:X:")
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
        opts.pmSetLongOption("raw", 0, "r", "", "output raw counter values (no rate conversion)")
        opts.pmSetLongOption("ignore-incompat", 0, "I", "", "ignore incompatible instances (default: abort)")
        opts.pmSetLongOption("instances", 1, "i", "STR", "instances to report (default: all current)")
        opts.pmSetLongOption("omit-flat", 0, "v", "", "omit single-valued metrics with -i (default: include)")
        opts.pmSetLongOption("precision", 1, "P", "N", "N digits after the decimal separator (default: 3)")
        opts.pmSetLongOption("count-scale", 1, "q", "SCALE", "default count unit")
        opts.pmSetLongOption("space-scale", 1, "b", "SCALE", "default space unit")
        opts.pmSetLongOption("time-scale", 1, "y", "SCALE", "default time unit")

        opts.pmSetLongOption("db-server", 1, "g", "SERVER", "InfluxDB server URL (default: " + SERVER + ")")
        opts.pmSetLongOption("db-name", 1, "x", "DATABASE", "metrics database name (default: " + DB + ")")
        opts.pmSetLongOption("db-user", 1, "U", "USERNAME", "username for database")
        opts.pmSetLongOption("db-pass", 1, "E", "PASSWORD", "password for database")
        opts.pmSetLongOption("db-tags", 1, "X", "TAGS", "string of tags to add to the metrics")

        return opts

    def option_override(self, opt):
        """ Override standard PCP options """
        if opt == 'H' or opt == 'K' or opt == 'g':
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
            self.precision = int(optarg)
        elif opt == 'q':
            self.count_scale = optarg
        elif opt == 'b':
            self.space_scale = optarg
        elif opt == 'y':
            self.time_scale = optarg
        elif opt == 'g':
            self.influx_server = optarg
        elif opt == 'x':
            self.influx_db = optarg
        elif opt == 'U':
            self.influx_user = optarg
        elif opt == 'E':
            self.influx_pass = optarg
        elif opt == 'X':
            self.influx_tags = optarg
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

        self.write_influxdb(tstamp)

    def write_header(self):
        """ Write info header """
        sys.stdout.write("Using database '%s' and tags '%s'.\n" % (self.influx_db, self.influx_tags))
        if self.context.type == PM_CONTEXT_ARCHIVE:
            sys.stdout.write("Sending %d archived metrics to InfluxDB at %s...\n(Ctrl-C to stop)\n" % (len(self.metrics), self.influx_server))
            return

        sys.stdout.write("Sending %d metrics to InfluxDB at %s every %d sec" % (len(self.metrics), self.influx_server, self.interval))
        if self.runtime != -1:
            sys.stdout.write(":\n%s samples(s) with %.1f sec interval ~ %d sec runtime.\n" % (self.samples, float(self.interval), self.runtime))
        elif self.samples:
            duration = (self.samples - 1) * float(self.interval)
            sys.stdout.write(":\n%s samples(s) with %.1f sec interval ~ %d sec runtime.\n" % (self.samples, float(self.interval), duration))
        else:
            sys.stdout.write("...\n(Ctrl-C to stop)\n")

    def write_influxdb(self, timestamp):
        """ Write (send) metrics to InfluxDB """
        if timestamp is None:
            # Silent goodbye
            return

        def sanitize_name_indom(string):
            """ Sanitize the instance domain string for InfluxDB """
            return "_" + re.sub('[^a-zA-Z_0-9-]', '_', string)

        # Prepare data for easier processing below
        metrics = []
        for metric in self.metrics:
            tmp = Metric(metric)
            try:
                for inst, name, val in self.metrics[metric][5](): # pylint: disable=unused-variable
                    if inst != PM_IN_NULL and not name:
                        continue
                    suffix = sanitize_name_indom(name) if name else "value"
                    try:
                        value = val()
                        value = round(value, self.precision) if isinstance(value, float) else value
                        tmp.add_field(suffix, value)
                    except:
                        pass
                metrics.append(tmp)
            except:
                pass

        ts = self.context.datetime_to_secs(self.pmfg_ts(), PM_TIME_NSEC)

        body = WriteBody()

        for metric in metrics:
            metric.set_tag_string(self.influx_tags)
            metric.set_timestamp(long(ts))
            body.add(metric)

        url = self.influx_server + '/write'
        params = {'db': self.influx_db}
        auth = None

        if self.influx_user and self.influx_pass:
            auth = requests.auth.HTTPBasicAuth(self.influx_user,
                                               self.influx_pass)

        try:
            res = requests.post(url, params=params, data=str(body), auth=auth)

            if res.status_code != 204:
                msg = "Could not send metrics: "

                if res.status_code == 200:
                    msg += "InfluxDB could not complete the request."
                elif res.status_code == 404:
                    msg += "Got an HTTP code 404. This most likely means "
                    msg += "that the requested database '"
                    msg += self.influx_db
                    msg += "' does not exist.\n"
                else:
                    msg += "request to "
                    msg += res.url
                    msg += " failed with code "
                    msg += str(res.status_code)
                    msg += ".\n"
                    msg += "Body of the request is:\n"
                    msg += str(body)
                    msg += "\n"

                sys.stderr.write(msg)
        except ValueError:
            sys.stderr.write("Can't send a request that has no metrics.\n")
        except requests.exceptions.ConnectionError as error:
            sys.stderr.write("Can't connect to InfluxDB server %s: %s, continuing.\n" % (self.influx_server, str(error.args[0].reason)))

    def finalize(self):
        """ Finalize and clean up """
        return

if __name__ == '__main__':
    try:
        P = PCP2InfluxDB()
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
