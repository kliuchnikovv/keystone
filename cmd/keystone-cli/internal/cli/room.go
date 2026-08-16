package cli

import "net/url"

// The daemon does not expose /rooms endpoints yet; the CLI hits the
// paths anyway so it will "just work" the moment the daemon lands
// them. Until then every verb returns 404 (exit code 3).

// RoomRoot dispatches "keystone room <verb> …".
func RoomRoot(args []string) error {
	if len(args) == 0 {
		return usageErr("room: verb required (list|create|rename|delete|devices)")
	}
	switch args[0] {
	case "list", "ls":
		return roomList(args[1:])
	case "create", "add":
		return roomCreate(args[1:])
	case "rename":
		return roomRename(args[1:])
	case "delete", "rm":
		return roomDelete(args[1:])
	case "devices":
		return roomDevices(args[1:])
	default:
		return usageErr("room: unknown verb %q", args[0])
	}
}

func roomList(_ []string) error {
	var rooms any
	if err := Get("/rooms", &rooms); err != nil {
		return err
	}
	Render(rooms)
	return nil
}

func roomCreate(args []string) error {
	if len(args) == 0 {
		return usageErr("room create <name>")
	}
	var resp map[string]any
	if err := Post("/rooms", map[string]any{"name": args[0]}, &resp); err != nil {
		return err
	}
	Render(resp)
	return nil
}

func roomRename(args []string) error {
	if len(args) < 2 {
		return usageErr("room rename <id> <newname>")
	}
	var resp map[string]any
	if err := Put("/rooms/"+url.PathEscape(args[0]), map[string]any{"name": args[1]}, &resp); err != nil {
		return err
	}
	Render(resp)
	return nil
}

func roomDelete(args []string) error {
	if len(args) == 0 {
		return usageErr("room delete <id>")
	}
	var resp map[string]any
	if err := Delete("/rooms/"+url.PathEscape(args[0]), &resp); err != nil {
		return err
	}
	Render(resp)
	return nil
}

func roomDevices(args []string) error {
	if len(args) == 0 {
		return usageErr("room devices <id>")
	}
	var resp any
	if err := Get("/rooms/"+url.PathEscape(args[0])+"/devices", &resp); err != nil {
		return err
	}
	Render(resp)
	return nil
}
