//go:build grpc

// keystone-testclient is a tiny gRPC smoke-test client used during POC. Build
// with `-tags=grpc`. Not part of the main product surface.
package main

import (
	"context"
	"encoding/json"
	"flag"
	"fmt"
	"log"
	"os"
	"time"

	"google.golang.org/grpc"
	"google.golang.org/grpc/credentials/insecure"

	pb "github.com/keystone/keystone/gen/go/keystone/v1"
)

func main() {
	addr := flag.String("addr", "localhost:7778", "gRPC server address")
	flag.Parse()

	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()

	conn, err := grpc.NewClient(*addr, grpc.WithTransportCredentials(insecure.NewCredentials()))
	if err != nil {
		log.Fatalf("dial: %v", err)
	}
	defer conn.Close()

	dev := pb.NewDeviceServiceClient(conn)
	rules := pb.NewRuleServiceClient(conn)

	// List devices.
	resp, err := dev.List(ctx, &pb.ListDevicesRequest{})
	if err != nil {
		log.Fatalf("List devices: %v", err)
	}
	fmt.Printf("Found %d devices:\n", len(resp.Devices))
	for _, d := range resp.Devices {
		fmt.Printf("  - %s | type=%s | transport=%s | features=%d\n",
			d.Name, d.Type, d.Transport, len(d.Features))
	}

	// List rules.
	rr, err := rules.ListRules(ctx, &pb.ListRulesRequest{})
	if err != nil {
		log.Fatalf("ListRules: %v", err)
	}
	fmt.Printf("Found %d rules:\n", len(rr.Rules))
	for _, r := range rr.Rules {
		fmt.Printf("  - %s | enabled=%v | %s\n", r.Name, r.Enabled, r.HumanReadable)
	}

	// Turn the first device on and immediately read back.
	if len(resp.Devices) > 0 {
		id := resp.Devices[0].Id
		fmt.Printf("Turning on device %s ...\n", id)
		if _, err := dev.InvokeAction(ctx, &pb.InvokeActionRequest{
			DeviceId: id, Feature: "onoff", Action: "turn_on",
		}); err != nil {
			log.Fatalf("InvokeAction: %v", err)
		}
		v, err := dev.ReadState(ctx, &pb.ReadStateRequest{
			DeviceId: id, Feature: "onoff", Key: "value",
		})
		if err != nil {
			log.Fatalf("ReadState: %v", err)
		}
		b, _ := json.Marshal(v.Value.AsInterface())
		fmt.Printf("  read back: %s\n", b)
	}

	// Stream initial + a few live snapshots.
	fmt.Println("Subscribing to state stream (5s)...")
	streamCtx, streamCancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer streamCancel()
	stateSvc := pb.NewStateServiceClient(conn)
	sub, err := stateSvc.Subscribe(streamCtx, &pb.SubscribeRequest{IncludeInitial: false})
	if err != nil {
		log.Fatalf("Subscribe: %v", err)
	}
	msgs := 0
	for {
		m, err := sub.Recv()
		if err != nil {
			break
		}
		msgs++
		if s := m.GetState(); s != nil {
			v, _ := json.Marshal(s.Value.AsInterface())
			fmt.Printf("  state: %s.%s.%s = %s\n", s.DeviceId, s.Feature, s.Key, v)
		}
		if e := m.GetEvent(); e != nil {
			fmt.Printf("  event: %s.%s\n", e.DeviceId, e.Name)
		}
		if msgs >= 5 {
			break
		}
	}
	fmt.Printf("Received %d messages\n", msgs)
	os.Exit(0)
}
