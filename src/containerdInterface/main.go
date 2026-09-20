// main.go
package main

/*
#include <stdlib.h>
#include <stdint.h>
*/
import "C"

import (
	"context"
	"fmt"
	"log"
	"unsafe"

	"github.com/containerd/containerd"
	"github.com/containerd/containerd/namespaces"
)

func main() {

}


//export GetContainerLabel
func GetContainerLabel(socketPath, ns, containerID, labelKey *C.char) *C.char {
	goSocket := C.GoString(socketPath)
	goNs := C.GoString(ns)
	goContainerID := C.GoString(containerID)
	goLabelKey := C.GoString(labelKey)

	ctx := namespaces.WithNamespace(context.Background(), goNs)
	client, err := containerd.New(goSocket)
	if err != nil {
		return nil
	}
	defer client.Close()

	container, err := client.LoadContainer(ctx, goContainerID)
	if err != nil {
		return nil
	}

	info, err := container.Info(ctx)
	if err != nil {
		return nil
	}

	val, ok := info.Labels[goLabelKey]
	if !ok {
		return nil
	}

	return C.CString(val)
}


//export getTagged
func getTagged(socketPath *C.char, ns *C.char, restorationHash *C.char) *C.char {
	//hopefully GC prevents blowups here
	containerdSock := C.GoString(socketPath)
	namespace := C.GoString(ns)
	restorationHashStr := C.GoString(restorationHash)

	ctx := namespaces.WithNamespace(context.Background(), namespace)
	client, err := containerd.New(containerdSock)
	if err != nil {
		return nil
	}
	defer client.Close()

	tagString := "restoreable==True"
	hashFilterString := fmt.Sprintf("image==%s", restorationHashStr)

	restorable, err := client.Containers(ctx, tagString, hashFilterString)
	if err != nil {
		log.Printf("GO ERROR| containerd had an error when applying filter string, err: %v", err)
		return nil
	}

	if restorable == nil {
		log.Printf("GO ERROR| containerd returned empty string in filter condition")
		return nil
	}

	//There could be multiple containers, i need ot make sure the controllers set the flag properly to avoid any screwups with running criu in the wrong container
	if len(restorable) > 1 {
		log.Printf("GO ERROR| multiple containers matched the filter condition")
		return nil
	}

	mount, err := client.SnapshotService("btrfs").Mounts(ctx, restorable[0].ID())
	img, _ := restorable[0].Image(ctx)
	if err != nil {
		log.Printf("GO ERROR| failed to get mounts for container %s: %v", img.Name(), err)
	}

	mountSource := mount[0].Source
	return C.CString(mountSource)
}

//export getRealPid
func getRealPid(socketPath *C.char, ns *C.char, containerID *C.char) *C.char {
	socketP := C.GoString(socketPath)
	namespace := C.GoString(ns)
	containerIDStr := C.GoString(containerID)

	ctx := namespaces.WithNamespace(context.Background(), namespace)
	client, err := containerd.New(socketP)
	if err != nil {
		return nil
	}
	defer client.Close()

	container, err := client.LoadContainer(ctx,containerIDStr)
	if err != nil {
		log.Printf("GO ERROR| failed to load container for PID read%s: %v", containerIDStr, err)
	}

	task, _ := container.Task(ctx,nil)
	pid := task.Pid()

	return C.CString(fmt.Sprintf("%d", pid))
}



//export FreeString
func FreeString(ptr *C.char) {
	if ptr != nil {
		C.free(unsafe.Pointer(ptr))
	}
}