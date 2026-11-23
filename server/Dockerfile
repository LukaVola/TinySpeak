FROM golang:1.27-alpine AS build
WORKDIR /src
COPY go.mod go.sum *.go ./
RUN CGO_ENABLED=0 go build -trimpath -ldflags="-s -w" -o /kanal-relay .

FROM alpine:3.22
RUN adduser -D -u 10001 kanal
COPY --from=build /kanal-relay /usr/local/bin/kanal-relay
USER kanal
ENTRYPOINT ["/usr/local/bin/kanal-relay"]
