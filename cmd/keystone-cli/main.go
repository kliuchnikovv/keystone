// keystone is the operator's and agent's CLI.
//
// Groups (plugin/device/system/self/stream) map onto the daemon's
// HTTP API one-to-one. JSON output is the default when stdout is not
// a terminal — pipes and scripts get a stable contract without a
// flag. Exit codes are stable per the spec so an agent can branch on
// them.
package main

import (
	"flag"
	"fmt"
	"os"

	"github.com/kliuchnikovv/keystone/cmd/keystone-cli/internal/cli"
)

const usage = `keystone — CLI for the keystone daemon

Usage:
  keystone [global-flags] <group> <verb> [args] [flags]
  keystone install <plugin>    (shortcut for: keystone plugin install)
  keystone uninstall <plugin>  (shortcut for: keystone plugin uninstall)
  keystone status              (shortcut for: keystone system status)

Groups:
  plugin     Install / enable / disable / restart / uninstall / info / logs
  device     List / get / set-state / invoke / delete
  system     Daemon health and info
  self       This CLI's version
  stream     Follow the live event stream (NDJSON)
  rule       List / get / create / delete / enable / disable / history / validate
  room       List / create / rename / delete / devices
  scene      List / create / apply / delete
  config     Get / set / unset / validate
  store      Browse / search / info / tap / untap / taps / categories
  secret     Set (stdin) / unset / list / rotate

Global flags:
  --host <url>         keystone HTTP endpoint (default $KEYSTONE_HOST or http://localhost:7777)
  --output, -o <fmt>   human | json  (default: human on tty, json off tty)
  --verbose, -v        Extra logging on stderr
  --help, -h           This help

Exit codes:
  0 ok · 1 error · 2 validation · 3 not-found · 4 auth · 5 timeout · 6 plugin · 7 conflict`

func main() {
	globals := parseGlobals(os.Args[1:])
	if len(globals.Args) == 0 {
		fmt.Fprintln(os.Stderr, usage)
		os.Exit(2)
	}
	cli.SetGlobal(globals.Global)

	cmd := globals.Args[0]
	args := globals.Args[1:]

	switch cmd {
	// Top-level shortcuts (§2 of the spec).
	case "install":
		cli.Exit(cli.PluginInstall(args))
	case "uninstall":
		cli.Exit(cli.PluginUninstall(args))
	case "status":
		cli.Exit(cli.SystemStatus(args))

	// Groups.
	case "plugin":
		cli.Exit(cli.PluginRoot(args))
	case "device":
		cli.Exit(cli.DeviceRoot(args))
	case "system":
		cli.Exit(cli.SystemRoot(args))
	case "self":
		cli.Exit(cli.SelfRoot(args))
	case "stream":
		cli.Exit(cli.StreamRoot(args))
	case "rule":
		cli.Exit(cli.RuleRoot(args))
	case "room":
		cli.Exit(cli.RoomRoot(args))
	case "scene":
		cli.Exit(cli.SceneRoot(args))
	case "config":
		cli.Exit(cli.ConfigRoot(args))
	case "store":
		cli.Exit(cli.StoreRoot(args))
	case "secret":
		cli.Exit(cli.SecretRoot(args))

	case "help", "--help", "-h":
		fmt.Println(usage)
	default:
		fmt.Fprintf(os.Stderr, "unknown command %q\n\n%s\n", cmd, usage)
		os.Exit(2)
	}
}

// parseGlobals strips the global flags off the front of the argv and
// returns them plus the remaining positional args. Group-local flag
// sets parse their own tail.
type parsed struct {
	Global cli.Global
	Args   []string
}

func parseGlobals(argv []string) parsed {
	fs := flag.NewFlagSet("keystone", flag.ExitOnError)
	fs.Usage = func() { fmt.Fprintln(os.Stderr, usage) }
	host := fs.String("host", os.Getenv("KEYSTONE_HOST"), "keystone HTTP endpoint")
	out := fs.String("output", "", "output format: human | json")
	outShort := fs.String("o", "", "output format (shorthand)")
	verbose := fs.Bool("verbose", false, "extra logging on stderr")
	fs.BoolVar(verbose, "v", false, "extra logging (shorthand)")

	// flag.FlagSet stops at the first non-flag positional, so
	// `keystone plugin list --output=json` reaches plugin_list with
	// the group-local flag intact. We only parse the leading globals
	// here.
	var remaining []string
	if len(argv) > 0 && isFlag(argv[0]) {
		if err := fs.Parse(argv); err != nil {
			os.Exit(2)
		}
		remaining = fs.Args()
	} else {
		remaining = argv
	}

	g := cli.Global{
		Host:    firstNonEmpty(*host, "http://localhost:7777"),
		Output:  firstNonEmpty(*out, *outShort),
		Verbose: *verbose,
	}
	if g.Output == "" {
		if isTTY(os.Stdout) {
			g.Output = "human"
		} else {
			g.Output = "json"
		}
	}
	return parsed{Global: g, Args: remaining}
}

func isFlag(s string) bool {
	return len(s) > 1 && s[0] == '-'
}

func firstNonEmpty(xs ...string) string {
	for _, s := range xs {
		if s != "" {
			return s
		}
	}
	return ""
}

func isTTY(f *os.File) bool {
	fi, err := f.Stat()
	if err != nil {
		return false
	}
	return fi.Mode()&os.ModeCharDevice != 0
}
