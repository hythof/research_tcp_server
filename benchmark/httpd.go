package main

import (
    "io"
    "net/http"
)

func handler(w http.ResponseWriter, r *http.Request) {
    io.WriteString(w, "ok")
}

func main() {
    http.HandleFunc("/", handler)
    http.ListenAndServe(":8880", nil)
}
