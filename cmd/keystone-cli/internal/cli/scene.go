package cli

import (
	"flag"
	"net/url"
	"strings"
)

// Same forward-compatibility posture as room: the daemon does not
// expose /scenes yet. When it does, this CLI just works.

// SceneRoot dispatches "keystone scene <verb> …".
func SceneRoot(args []string) error {
	if len(args) == 0 {
		return usageErr("scene: verb required (list|create|apply|delete)")
	}
	switch args[0] {
	case "list", "ls":
		return sceneList(args[1:])
	case "create", "add":
		return sceneCreate(args[1:])
	case "apply":
		return sceneApply(args[1:])
	case "delete", "rm":
		return sceneDelete(args[1:])
	default:
		return usageErr("scene: unknown verb %q", args[0])
	}
}

func sceneList(_ []string) error {
	var scenes any
	if err := Get("/scenes", &scenes); err != nil {
		return err
	}
	Render(scenes)
	return nil
}

func sceneCreate(args []string) error {
	fs := flag.NewFlagSet("scene create", flag.ContinueOnError)
	fromCurrent := fs.Bool("from-current", false, "snapshot current device state")
	rooms := fs.String("rooms", "", "comma-separated room ids to include")
	if err := fs.Parse(args); err != nil {
		return err
	}
	if fs.NArg() == 0 {
		return usageErr("scene create <name> [--from-current] [--rooms=r1,r2]")
	}
	body := map[string]any{"name": fs.Arg(0), "from_current": *fromCurrent}
	if *rooms != "" {
		body["rooms"] = splitCSV(*rooms)
	}
	var resp map[string]any
	if err := Post("/scenes", body, &resp); err != nil {
		return err
	}
	Render(resp)
	return nil
}

func sceneApply(args []string) error {
	if len(args) == 0 {
		return usageErr("scene apply <id>")
	}
	var resp map[string]any
	if err := Post("/scenes/"+url.PathEscape(args[0])+"/apply", nil, &resp); err != nil {
		return err
	}
	Render(resp)
	return nil
}

func sceneDelete(args []string) error {
	if len(args) == 0 {
		return usageErr("scene delete <id>")
	}
	var resp map[string]any
	if err := Delete("/scenes/"+url.PathEscape(args[0]), &resp); err != nil {
		return err
	}
	Render(resp)
	return nil
}

func splitCSV(s string) []string {
	parts := strings.Split(s, ",")
	out := parts[:0]
	for _, p := range parts {
		p = strings.TrimSpace(p)
		if p != "" {
			out = append(out, p)
		}
	}
	return out
}
