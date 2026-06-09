module golem-webtransport-interop-server

go 1.25.4

require golem-engine v0.0.0

require (
	github.com/coder/websocket v1.8.14 // indirect
	github.com/dunglas/httpsfv v1.1.0 // indirect
	github.com/quic-go/qpack v0.6.0 // indirect
	github.com/quic-go/quic-go v0.59.0 // indirect
	github.com/quic-go/webtransport-go v0.10.0 // indirect
	golang.org/x/crypto v0.41.0 // indirect
	golang.org/x/net v0.43.0 // indirect
	golang.org/x/sys v0.35.0 // indirect
	golang.org/x/text v0.28.0 // indirect
)

replace golem-engine => C:/Demiurgos/golem-engine

replace golem.collision => C:/Demiurgos/golem-engine/golem/collision

replace golem.collision/cp => C:/Demiurgos/golem-engine/golem/collision/cp

replace golem.collision/resolv => C:/Demiurgos/golem-engine/golem/collision/resolv

replace golem.nav => C:/Demiurgos/golem-engine/golem/nav

replace golem.nav/pathing => C:/Demiurgos/golem-engine/golem/nav/pathing

replace golem.nav/kelindar => C:/Demiurgos/golem-engine/golem/nav/kelindar
