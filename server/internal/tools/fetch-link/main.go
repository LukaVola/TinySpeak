// Downloads the pinned Windows build of the iroh transport during packaging.
package main

import (
	"archive/zip"
	"bytes"
	"crypto/sha256"
	"fmt"
	"io"
	"log"
	"net/http"
	"os"
	"path/filepath"
	"time"
)

const source = "https://github.com/n0-computer/dumbpipe/releases/download/v0.40.2/dumbpipe-v0.40.2-windows-x86_64.zip"
const checksum = "3874c0c1ebe9308f891bc57eb1a4e822697fc82ecd7de494ad74bc327c3ce87a"

func download(output string) error {
	client := http.Client{Timeout: 60 * time.Second}
	res, err := client.Get(source)
	if err != nil {
		return err
	}
	defer res.Body.Close()
	if res.StatusCode != http.StatusOK {
		return fmt.Errorf("download failed: %s", res.Status)
	}
	data, err := io.ReadAll(io.LimitReader(res.Body, 8<<20))
	if err != nil {
		return err
	}
	if fmt.Sprintf("%x", sha256.Sum256(data)) != checksum {
		return fmt.Errorf("transport checksum mismatch")
	}
	archive, err := zip.NewReader(bytes.NewReader(data), int64(len(data)))
	if err != nil {
		return err
	}
	for _, entry := range archive.File {
		if filepath.Base(entry.Name) != "dumbpipe.exe" {
			continue
		}
		in, err := entry.Open()
		if err != nil {
			return err
		}
		defer in.Close()
		binary, err := io.ReadAll(in)
		if err != nil {
			return err
		}
		return os.WriteFile(filepath.Join(output, "TinySpeak Link.exe"), binary, 0755)
	}
	return fmt.Errorf("Windows transport missing from verified archive")
}

func main() {
	if len(os.Args) != 2 {
		log.Fatal("usage: fetch-link OUTPUT_DIRECTORY")
	}
	if err := download(os.Args[1]); err != nil {
		log.Fatal(err)
	}
}
