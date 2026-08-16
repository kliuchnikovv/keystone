// keystone-plugin is the plugin author's local CLI: new, build, test,
// publish. Each subcommand lives in its own file; this dispatcher is
// deliberately dumb so a first-time reader can trace what a command
// does from one function.
package main

import (
	"fmt"
	"os"
)

const usage = `keystone-plugin — the plugin author's local CLI

Usage:
  keystone-plugin <command> [flags]

Commands:
  new       Scaffold a new plugin directory
  build     Compile the plugin into bin/
  test      Validate manifest and run go test ./...
  publish   Pack the plugin into a distributable tarball
  version   Print the CLI version

Use "keystone-plugin <command> -h" for command-specific flags.`

const Version = "0.1.0"

func main() {
	if len(os.Args) < 2 {
		fmt.Fprintln(os.Stderr, usage)
		os.Exit(2)
	}
	cmd := os.Args[1]
	args := os.Args[2:]
	switch cmd {
	case "new":
		exit(runNew(args))
	case "build":
		exit(runBuild(args))
	case "test":
		exit(runTest(args))
	case "publish":
		exit(runPublish(args))
	case "version", "-v", "--version":
		fmt.Println(Version)
	case "help", "-h", "--help":
		fmt.Println(usage)
	default:
		fmt.Fprintf(os.Stderr, "unknown command %q\n\n%s\n", cmd, usage)
		os.Exit(2)
	}
}

func exit(err error) {
	if err == nil {
		return
	}
	fmt.Fprintln(os.Stderr, err)
	os.Exit(1)
}
