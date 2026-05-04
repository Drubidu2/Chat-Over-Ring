# COR, Chat over a Ring with Chords

## Overview

COR is a distributed chat application written in C for the RCI 2023/2024 project, "Chat Sobre Anel". Each running instance represents one node in an overlay network. The base topology is a directed ring, and the ring may be extended with TCP chords between non adjacent nodes.

The application supports three main layers:

1. **Chat layer**: sends user messages between nodes.
2. **Routing layer**: computes shortest paths and forwards messages through the best available neighbour.
3. **Topology layer**: maintains the ring structure when nodes join, leave, or create and remove chords.

## Main Features

1. Creation of a ring with one or more nodes.
2. Node insertion through a registry based `join` command.
3. Node insertion through a direct `direct join` command.
4. Graceful node departure with ring preservation.
5. Creation and removal of chords between non adjacent nodes.
6. Shortest path routing using local routing and shortest path tables.
7. Forwarding table generation for chat message delivery.
8. TCP communication between neighbouring nodes and chord endpoints.
9. UDP communication with the node registry server.
10. UDP timeout handling when waiting for registry server replies.

## Repository Structure

```text
.
├── COR.c       Main implementation: command parser, TCP and UDP communication, topology handling, routing, forwarding, and chat delivery
├── COR.h       Data structures, constants, and function declarations
├── Makefile    Build rules for the COR executable
└── README.md   Project documentation
```

## Requirements

The project is intended to run on a Unix or Linux environment with POSIX sockets.

Required tools:

1. `gcc`
2. `make`
3. Network access between all node instances
4. Access to the registry server when using `join` or `chord`

Default registry server:

```text
IP: 193.136.138.142
UDP port: 59000
```

## Build

Compile the project with:

```bash
make
```

Remove generated files with:

```bash
make clean
```

### Build note for case sensitive file systems

The uploaded source file includes the header as:

```c
#include "cor.h"
```

If the header file is named `COR.h`, compilation on Linux may fail because filenames are case sensitive. Use one of the following fixes before running `make`:

```bash
cp COR.h cor.h
make
```

or change the include directive in `COR.c` to:

```c
#include "COR.h"
```

## Running a Node

The executable syntax follows the project specification:

```bash
./COR <IP> <TCP_PORT> [REGISTRY_IP] [REGISTRY_UDP_PORT]
```

Arguments:

| Argument | Description |
| --- | --- |
| `IP` | IP address of the machine running the node |
| `TCP_PORT` | TCP listening port used by this node |
| `REGISTRY_IP` | Optional IP address of the registry server |
| `REGISTRY_UDP_PORT` | Optional UDP port of the registry server |

Example using the default registry server:

```bash
./COR 127.0.0.1 5001
```

Example with explicit registry parameters:

```bash
./COR 127.0.0.1 5001 193.136.138.142 59000
```

## User Commands

| Command | Short form | Syntax | Description |
| --- | --- | --- | --- |
| `join` | `j` | `j <ring> <id>` | Join a ring using the registry server |
| `direct join` | `dj` | `dj <id> <succid> <succIP> <succTCP>` | Join a ring directly through a known successor |
| `chord` | `c` | `c` | Create one chord to a non adjacent node |
| `remove chord` | `rc` | `rc` | Remove the chord created by this node |
| `show topology` | `st` | `st` | Display predecessor, self node, successor, second successor, and active chords |
| `show routing` | `sr` | `sr <dest>` | Display routing table entries for a destination |
| `show path` | `sp` | `sp <dest>` | Display the shortest known path to a destination |
| `show forwarding` | `sf` | `sf` | Display the forwarding table |
| `message` | `m` | `m <dest> <message>` | Send a chat message to another node |
| `leave` | `l` | `l` | Leave the ring gracefully |
| `exit` | `x` | `x` | Leave the ring if joined, close sockets, and terminate the application |

There is also a debug command in the source code:

```text
clean <ring> <id>
```

This command unregisters a specific node from the registry server and should only be used for testing or cleanup.

## Identifier Rules

Ring identifiers use three digits:

```text
000 to 999
```

Node identifiers use two digits:

```text
00 to 99
```

The implementation stores up to `MAX_NODES` nodes, currently set to 16.

## Example Local Test

Open one terminal per node.

Terminal 1:

```bash
./COR 127.0.0.1 5001
j 045 01
st
```

Terminal 2:

```bash
./COR 127.0.0.1 5002
j 045 02
st
sf
```

Terminal 3:

```bash
./COR 127.0.0.1 5003
j 045 03
st
sf
```

Send a chat message from node `02` to node `01`:

```text
m 01 Hello from node 02
```

Create a chord from the current node:

```text
c
```

Inspect the updated routing state:

```text
sr 01
sp 01
sf
```

Remove the chord:

```text
rc
```

Leave the ring:

```text
l
```

Terminate the application:

```text
x
```

## Protocol Summary

The implementation uses TCP messages between overlay neighbours and chord endpoints. It also uses UDP messages to communicate with the registry server.

### Topology Messages

| Message | Purpose |
| --- | --- |
| `ENTRY <id> <ip> <tcp>` | Request insertion before a known successor |
| `PRED <id>` | Inform a node about its predecessor |
| `SUCC <id> <ip> <tcp>` | Inform a node about its successor or second successor |
| `CHORD <id>` | Establish a chord adjacency |

### Registry Messages

| Message | Purpose |
| --- | --- |
| `NODES <ring>` | Request the nodes currently registered in a ring |
| `REG <ring> <id> <ip> <tcp>` | Register a node in a ring |
| `UNREG <ring> <id>` | Remove a node from the registry |

### Routing Messages

Routing updates use messages of the form:

```text
ROUTE <origin> <destination> <path>
```

When a destination becomes unreachable through a neighbour, the implementation may advertise a route withdrawal using:

```text
ROUTE <origin> <destination>
```

Paths are represented as ordered node identifiers separated by `-`, for example:

```text
01-03-07
```

### Chat Messages

Chat messages use the following format:

```text
CHAT <origin> <destination> <message>
```

The project statement limits chat messages to 128 characters.

## Routing Behaviour

Each node maintains:

1. A routing table indexed by destination and neighbour.
2. A shortest path table containing the currently selected best path for each destination.
3. A forwarding table mapping each destination to the next hop neighbour.

When a new adjacency is added, the node exchanges its known shortest paths with the new neighbour. When an adjacency is removed, the corresponding routing entries are removed, shortest paths are recomputed, and changed routes are propagated to the remaining neighbours.

Chat delivery uses the forwarding table. If the current node is not the destination, the message is forwarded to the next hop on the shortest known path.

## Notes and Limitations

1. The application is interactive and expects commands from standard input.
2. Each node process should use a distinct TCP port.
3. Chords are established only to nodes that are not the current node, predecessor, or successor.
4. Each node creates at most one outgoing chord, although it may receive several incoming chord connections.
5. UDP communication with the registry server uses a 5 second timeout.
6. The routing state is eventually updated through propagated `ROUTE` messages after topology changes.
7. The program should be terminated with `leave` or `exit` to allow graceful cleanup.

## Suggested Test Checklist

1. Start one node and join an empty ring.
2. Add a second node and verify `show topology` on both nodes.
3. Add more nodes and verify predecessor, successor, and second successor values.
4. Create a chord and verify that routing and forwarding tables are updated.
5. Send chat messages between non adjacent nodes.
6. Remove a chord and verify that shortest paths are recomputed.
7. Remove a node using `leave` and confirm that the ring remains connected.
8. Test UDP timeout behaviour by using an unreachable registry address.

## License

This project was developed for academic purposes. No license is specified.
