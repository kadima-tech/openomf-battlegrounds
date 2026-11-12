# The OpenBG lobby server as a container: a static binary on an empty image.
#
#   docker build -t openbg-lobby server/
#   docker run -p 27900:27900/udp -p 27901-27964:27901-27964/udp openbg-lobby
#
# Settings are the binary's flags or OPENBG_LOBBY_* environment variables (see DEVELOPING.md).
FROM golang:1.22-alpine AS build
WORKDIR /src
COPY go.mod ./
COPY . .
RUN go test ./... && CGO_ENABLED=0 go build -trimpath -ldflags="-s -w" -o /openbg-lobby ./cmd/openbg-lobby

FROM scratch
COPY --from=build /openbg-lobby /openbg-lobby
EXPOSE 27900/udp 27901-27964/udp
USER 65534:65534
ENTRYPOINT ["/openbg-lobby"]
CMD ["-listen", ":27900", "-relay-ports", "27901-27964"]
