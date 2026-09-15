# Fabric Topology

**Fabric Topology** is the authoritative, generation-bound topology-governance runtime of the
Distributed Fabric Infrastructure / Fabric OS stack. It answers one question and answers it
precisely:

> What fabric entities are connected to what, through which current relationships, under which
> topology generation, provenance and control-plane authority — and when must a topology
> relationship, topology snapshot or topology-derived claim be rejected as stale, conflicting,
> invalid, incomplete or non-authoritative?

It is a vendor-neutral C++20 library plus a small set of operational tools. It models structure.
It does not model health, reachability, routing or path authority.

---

## 1. Where Fabric Topology sits

Fabric Topology consumes canonical identities established by **Fabric Registry** and builds a
governed graph over them.

| Concern | Owner |
| --- | --- |
| Canonical entity identity (FabricId, SiteId, DeviceId, SwitchId, RouterId, NicId, PortId, LinkId, EndpointId, ControlParticipantId) | **Fabric Registry** |
| Current link up/down/degraded evidence | **Link State Fabric** |
| Port configuration and deeper operational semantics | **Port Fabric** |
| Protocol, speed, queue, forwarding, offload and telemetry capability truth | **Fabric Capability Registry** |
| Failure-domain semantics | **Failure Domain Registry** |
| Global control-plane epoch authority when separated | **Fabric Epoch** |
| Whether a path is currently legally usable | **Path Authority** |
| Routes, path planning, ECMP, traffic engineering, admission, congestion, bandwidth reservation, queues, failover, flow scheduling | later runtimes (not here) |
| **Topology structure, generation, provenance, currentness, reconciliation, snapshots, diffs, explanations** | **Fabric Topology (this repository)** |

### Systems boundary

Fabric Topology **owns**:

topology nodes and edges; endpoint, attachment, containment and connectivity relationships;
network and fabric tiers; topology domains and their authority scopes; site and fabric membership
relationships; physical and logical topology relationships; topology generations; topology
snapshots and their currentness; topology mutation authority; topology provenance; topology
validation and structural invariants; topology reconciliation; stale-topology fencing;
deterministic topology explanations.

Fabric Topology **does not own**: canonical identity creation; dynamic link health; port
configuration beyond endpoint identification; device capability truth; failure-domain semantics;
global epoch authority when Fabric Epoch is separate; and it never computes or authorises a path.

### The dependency seam

Canonical identity is reached through `fabric_topology::IEntityDirectory`:

    std::optional<EntityRecord> lookup(std::string_view entity_id) const;

An `EntityRecord` reports the canonical identifier, its `EntityClass`, its
`EntityGeneration`, and whether it is retired or superseded (and by whom). Fabric Topology
validates references through this seam and never becomes the authority for identity ownership.
`InMemoryEntityDirectory` is the reference adapter used by the tests, the examples, the tools
and the multi-process proofs; `runtime::load_registry_seed` and
`runtime::write_registry_seed` are the shipped file-backed adapter so a coordinator process
can hold exactly the identities its publishers reference.

---

## 2. Architectural principle

Topology is never confused with identity, health, reachability, routing or path authority.

A topology edge means: *these endpoints are related by this governed topology relationship under
this topology generation and current authority.* It does **not** mean the link is up, the link is
healthy, traffic can pass, the route is selected, the path is permitted, bandwidth is available,
latency is acceptable, or that the relationship stays physically valid forever.

The runtime therefore keeps five dimensions separate at all times:

* **structure** — the graph of nodes and relationships;
* **operational condition** — not modelled here at all;
* **provenance** — who asserted a fact, from which evidence, under which incarnation;
* **generation** — which registry and topology generation a fact is bound to;
* **authority and currentness** — whether the fact is current, needs revalidation, has been
  superseded, retired or conflicted, or is simply unknown.

`UNKNOWN` is a first-class answer and is never silently folded into "absent".

---

## 3. Graph model

Topology is a governed graph, never a flat list of devices.

### Node classes

`Site`, `Fabric`, `RackNetworkDomain`, `Switch`, `Router`,
`Nic`, `SmartNic`, `DpuEndpoint`, `PhysicalPort`, `LogicalPort`,
`FabricEndpoint`, `LogicalEndpoint`.

Every node class maps onto exactly one Fabric Registry entity class:

| Node class | Registry entity class |
| --- | --- |
| Site, RackNetworkDomain | `SITE` |
| Fabric | `FABRIC` |
| Switch | `SWITCH` |
| Router | `ROUTER` |
| Nic | `NIC` |
| SmartNic | `SMARTNIC` |
| DpuEndpoint | `DPU` |
| PhysicalPort | `PORT` |
| LogicalPort | `LOGICAL_PORT` |
| FabricEndpoint, LogicalEndpoint | `ENDPOINT` |

There is no synthetic "aggregation" node class: no such node can be justified against registry
identity, so the model does not offer one. One canonical identity participates in the runtime as
at most one topology node; multi-scope participation is expressed with membership relationships,
not by duplicating nodes.

### Relation classes

Thirteen relation classes, each with fixed, documented semantics. Authoritative state cannot
contain an untyped relationship.

| Relation | Direction | Layer policy | Cycle rule | From to To (node classes) |
| --- | --- | --- | --- | --- |
| `CONNECTED_TO` | undirected | physical | cycles allowed | ports/endpoints to ports/endpoints |
| `ATTACHED_TO` | directed | physical | cycles allowed | NIC/SmartNIC/DPU/port to switch/router/port |
| `CONTAINS` | directed | either | **acyclic** | site/fabric/rack/switch/router to any node |
| `MEMBER_OF` | directed | either | **acyclic** | site/rack/switch/router/NIC/port/endpoint to fabric/site/rack |
| `UPLINK_TO` | directed | physical | cycles allowed | switch/router/port to switch/router/port |
| `DOWNLINK_TO` | directed | physical | cycles allowed | switch/router/port to switch/router/port |
| `PEERS_WITH` | undirected | logical | cycles allowed | switch/router to switch/router |
| `BACKED_BY` | directed | logical | **acyclic** | logical port/endpoint to port/NIC/SmartNIC/DPU/switch/router/endpoint |
| `TUNNELED_OVER` | directed | logical | cycles allowed | port/endpoint to port/endpoint/switch/router |
| `LOGICALLY_CONNECTED_TO` | undirected | logical | cycles allowed | ports/endpoints to ports/endpoints |
| `HOSTED_BY` | directed | physical | **acyclic** | NIC/SmartNIC/DPU/port/switch to switch/router/NIC/SmartNIC/DPU |
| `PRESENTS_ENDPOINT` | directed | physical | **acyclic** | NIC/SmartNIC/DPU/switch/router/port to port/endpoint |
| `FABRIC_MEMBERSHIP` | directed | logical | **acyclic** | site/rack/switch/router to fabric |

Direction and cycle rules are properties of the relation class, so no caller has to infer either
from a name. Container-style relations are acyclic; physical connectivity may legitimately form
cycles; a whole-graph cycle ban would be wrong in both directions, so neither is applied globally.

Undirected relationships are stored in one normalized orientation, so one structural relationship
has exactly one authoritative edge and can never be declared twice.

### Tiers

`Unspecified`, `Endpoint`, `Access`, `TopOfRack`, `Leaf`,
`Spine`, `SuperSpine`, `Border`, `Gateway`, `SiteEdge`,
`InterSite`, `OpticalInterconnect`.

Tiers are descriptive labels over general graph semantics. No architecture is mandatory or
hard-coded: spine-leaf, fat-tree, Clos, mesh, ring, direct-connect, host-connected, hierarchical
and synthetic test topologies are all representable, and none of them is required.

### Domains and scopes

`ScopeKind` is one of `Fabric`, `Site`, `AdministrativeDomain`,
`PhysicalDomain`, `LogicalDomain`, `ControlPlaneDomain`. Every relationship
belongs to exactly one authoritative scope. A relationship that legitimately spans two scopes must
be requested as cross-domain and is only accepted when the engine holds an explicit
`CrossDomainRule` for that ordered (or symmetric) domain pair and relation class; the edge
then records that it is cross-domain and which secondary scope it involves. Accidental
cross-domain edge creation is impossible.

---

## 4. Physical versus logical topology

The two layers are never flattened into one ambiguous graph.

* Physical: `CONNECTED_TO`, `ATTACHED_TO`, `UPLINK_TO`, `DOWNLINK_TO`,
  `HOSTED_BY`, `PRESENTS_ENDPOINT`; cable and link endpoint pairing, NIC-to-switch
  attachment, switch-to-switch attachment, port-to-port connection, device containment.
* Logical: `PEERS_WITH`, `BACKED_BY`, `TUNNELED_OVER`,
  `LOGICALLY_CONNECTED_TO`, `FABRIC_MEMBERSHIP`; virtual interfaces, bonds and teams,
  logical aggregation, tunnels, overlays, virtual fabrics, logical endpoint membership.
* Either: `CONTAINS`, `MEMBER_OF`. When a relation permits both layers the caller
  **must** state the layer explicitly; the runtime never guesses.

A logical relationship can carry `supported_by`, pointing at the physical relationship it
derives from, when the publisher supplies one. Fabric Topology never invents the supporting path,
and it never computes the path a tunnel takes through the fabric.

---

## 5. Generation model

Generation kinds are distinct types and are never interchangeable.

| Type | Meaning |
| --- | --- |
| `TopologyGeneration` | global authoritative topology generation |
| `NodeGeneration` | per-node topology generation |
| `EdgeGeneration` | per-relationship topology generation |
| `SnapshotGeneration` | snapshot ordinal |
| `EvidenceGeneration` | position in a publisher's evidence stream |
| `EntityGeneration` | incarnation of a canonical Fabric Registry identity |
| `CoordinatorEpoch` | control-plane epoch of the coordinator |

Rules, enforced by construction:

* the topology generation advances by **exactly one** when authoritative topology state actually
  changes;
* an idempotent replay never advances it;
* every mutation may state the generation it expects; a stale expectation is rejected **before**
  any state is touched;
* a relationship records the entity generations of both endpoints at bind time; a relationship
  bound to an older entity generation can never silently become current again;
* durable recovery is not a mutation. It preserves the recovered generation and may only *lower*
  currentness. The resulting digest change is visible to snapshot currentness checks.

---

## 6. Authority, incarnation fencing and epochs

Every mutation and publication carries an `AuthorityContext`: coordinator epoch, publisher,
worker boot identifier, evidence generation, optional mutation attempt and publication identity.
All of it is checked before any state is touched.

* **Scope grants.** A publisher holds `None`, `Read`, `IncrementalWrite` or
  `AuthoritativeWrite` per scope. The coordinator grants at most what its configured
  delegation policy allows; a publisher can never widen its own scope, and publishing outside its
  granted scope is refused with `UNAUTHORIZED_SCOPE`.
* **Worker incarnation fencing.** A restarted worker receives a fresh `WorkerBootId`; the
  previous one is permanently stale. Old-boot traffic cannot add edges, delete edges, refresh
  evidence, replace attachments, supersede newer topology, renew currentness or restore retired
  relationships. Fencing also demotes the relationships that incarnation asserted to
  `REVALIDATION_REQUIRED`: durable structure survives, live currentness does not.
* **Coordinator epoch.** A fresh coordinator start advances the epoch, clears all process-local
  publisher authority and moves every publisher-asserted relationship to
  `REVALIDATION_REQUIRED`. Publishers must re-establish authority under the new epoch.
  Old-epoch traffic is refused with `STALE_COORDINATOR_EPOCH`.

These are proven with real OS processes (see section 11).

---

## 7. Currentness

`LifecycleState`: `UNKNOWN`, `CURRENT`, `REVALIDATION_REQUIRED`,
`SUPERSEDED`, `RETIRED`, `CONFLICTED`.

Durable and current are different things. A recovered relationship is durable but not
automatically current. A retired or superseded relationship stays inspectable forever and can
never be revived. Revalidation is refused with `STALE_ENTITY_GENERATION` when the registry
has moved on from the entity generation a node is still bound to.

Queries distinguish: no relationship exists, relationship is known absent, relationship is
unknown, relationship exists but is stale, relationship exists and is current.

---

## 8. Publication, reconciliation and authoritative snapshots

A publication is an untrusted assertion about part or all of one authority scope. Three modes,
with materially different semantics:

| Mode | Adds/updates | Explicit `present=false` | Unmentioned relationships | Node removal | Generation expectation |
| --- | --- | --- | --- | --- | --- |
| `INCREMENTAL` | yes | honoured (retires) | untouched | not allowed | if non-zero, must match |
| `PARTIAL_OBSERVATION` | yes | rejected entry | untouched | not allowed | if non-zero, must match |
| `AUTHORITATIVE_SNAPSHOT` | yes | honoured (removes) | removed if in scope | honoured | if non-zero, must match |

An incremental or partial source can never implicitly delete an unmentioned edge. An authoritative
snapshot is transactional: the runtime validates publisher authority, scope, every referenced
identity, every relationship and the complete candidate graph, then commits atomically and
advances the generation once. A single invalid entry rejects the whole publication; half a
snapshot is never committed.

Nodes that would otherwise be orphaned by a snapshot replacement are retired rather than erased,
so no authoritative relationship is ever left dangling.

Stale evidence is handled precisely: an authoritative snapshot that publishes an evidence
generation older than what is stored is rejected (`STALE_GENERATION`); an incremental or
partial publication leaves the newer relationship untouched and reports the stale entries.

---

## 9. Snapshots, digests, diffs and explanations

* `snapshot(scope)` returns an immutable value that owns copies of its content, binding the
  snapshot ordinal, topology generation, coordinator epoch, scope and a deterministic SHA-256
  digest. Content and digest are built under a shared lock; the exclusive lock is only taken to
  allocate the ordinal and record history, so snapshot construction does not block readers.
* `check_snapshot(snapshot)` reports `CURRENT`, `OLDER_GENERATION`,
  `NEWER_GENERATION`, `DIFFERENT_COORDINATOR_EPOCH` or `DIGEST_MISMATCH`. Old
  snapshots stay inspectable but never masquerade as current.
* The **digest** is computed over canonical little-endian bytes with total ordering of nodes,
  edges, relations, metadata, scopes, enums and optional fields. Process-local incarnation data
  (publisher, worker boot, coordinator epoch, publication identity, evidence stream position and
  validation bookkeeping) is deliberately excluded, so an independent encoding of the same
  authoritative topology produces the same digest. Insertion order does not affect it.
* **Diffs** (`NODE_ADDED`, `NODE_REMOVED`, `NODE_GENERATION_CHANGED`,
  `EDGE_ADDED`, `EDGE_REMOVED`, `EDGE_SUPERSEDED`, `ATTACHMENT_MOVED`,
  `RELATIONSHIP_CHANGED`, `SCOPE_CHANGED`, `CURRENTNESS_CHANGED`) are sorted by
  (kind, key) and carry their own digest. No unordered container iteration order ever reaches an
  externally visible result.
* **Explanations** are structured, sorted (key, value) records with a stable machine code and a
  deterministic rendering. `explain_relationship`, `explain_node` and
  `explain_generation` answer *why* a decision happened, including the currentness reason
  and the generations involved.

---

## 10. Persistence and recovery

Durable topology uses a versioned, integrity-checked container:

    magic blob (4-byte length prefix + 8 magic bytes)
    format version (u32)
    payload length (u64)
    payload CRC-32 (u32)
    payload SHA-256 (4-byte length prefix + 32 bytes)
    payload

The payload is a deterministic encoding of the topology generation, snapshot counter, coordinator
epoch, every node, every edge, every domain, every cross-domain rule and the permanently fenced
worker boot identifiers. Publisher registrations are never persisted as live authority.

The decoder is bounded and checked: declared counts are validated against the configured limits
before any allocation, arithmetic is checked, and duplicate nodes and edges, unknown enums,
malformed identifiers, dangling endpoints, unknown domains, trailing bytes, truncation, bad magic,
bad version, bad CRC and bad SHA-256 are all rejected with `PERSISTENCE_CORRUPTION` and a
precise code. Decoded state is additionally run through the full structural validation, so a
containment cycle encoded on disk is refused rather than loaded. Nothing partially written is ever
exposed as authoritative: the container is written to a temporary file and atomically renamed.

Recovery is conservative: structure survives, live authority does not, the coordinator epoch
advances, publisher-asserted evidence becomes `REVALIDATION_REQUIRED`, nodes whose registry
generation has moved on become `REVALIDATION_REQUIRED`, and permanently fenced worker boots
stay fenced.

`PersistenceMode::Immediate` keeps memory and disk in step: a mutation is rolled back if its
durable write fails, so the two can never disagree.

---

## 11. Distributed publication

Where distributed publication exists, it uses real independent OS processes over real sockets.
`ftcoordinator` hosts a `TopologyEngine`; `ftpublisher` connects, registers and
publishes.

* **Framed protocol.** A 28-byte fixed-layout header (4-byte length prefix plus 4 magic bytes,
  version, message type, flags, sequence, payload length, CRC-32) followed by the payload. Raw C++
  memory layouts are never transmitted: every field is written explicitly, little-endian.
* **Validation.** Unsupported version, unknown message type, oversized declared payload, CRC
  mismatch, truncated frame and malformed payload are all rejected; a mismatched peer is
  disconnected without disturbing the coordinator.
* **Authority binding.** The coordinator binds authority to the *session*, never to what the client
  claims about itself. A client that presents a coordinator epoch other than the live one is
  refused in the handshake.
* **Worker death.** When a publisher process dies, the session's socket dies, the session ends,
  and the coordinator fences that worker boot through the real control path and demotes the
  relationships it asserted.
* **Shutdown.** A receive never blocks indefinitely: the socket is polled with a short interval and
  an explicit stop flag, so `shutdown()` semantics are never depended on. A session thread
  never joins itself, and a session thread never tears down the coordinator it runs inside.

### Proof obligations exercised with real processes

`ft_test_distributed` spawns real `ftcoordinator` and `ftpublisher` processes and
terminates them with `TerminateProcess`:

1. publisher A and publisher B publish into two independent scopes;
2. A is killed without any graceful shutdown;
3. the loss is detected through the session socket, A's worker boot is fenced, and A's
   relationships become `REVALIDATION_REQUIRED`;
4. B is unaffected and its relationships stay current;
5. A-prime starts with a fresh worker boot, re-establishes authority and republishes without
   duplicating canonical relationships;
6. the coordinator is killed and restarted against the same durable image: the epoch advances,
   durable structure is preserved at the recovered generation, no relationship is current, the
   previous epoch is refused, the fenced worker boot is still refused, and a fresh publisher
   commits the next generation;
7. malformed and truncated peers never disturb the coordinator;
8. repeated start/stop cycles leave no orphan process.

---

## 12. Resource bounds and security

Publications, protocol frames, persistence containers and API requests are all untrusted input.

Explicit limits bound identifier length, string length, metadata entries/keys/values and total
size, nodes and edges per publication, publication diff entries, queued publications, graph size,
degree, snapshot size, traversal depth and visited set, persistence size and record count, frame
size and session count. Every externally influenced count is validated with checked arithmetic
before any allocation; memory is never reserved directly from an untrusted declared count.

Graph algorithms are iterative. Containment and dependency acyclicity is enforced with an explicit
work stack, so a deep chain cannot exhaust the stack. Acyclicity maintenance is the standard
`O(V + E_rel)` per insert over that relation class; every other index operation is
`O(1)`, and indexes are never rebuilt per mutation.

---

## 13. Concurrency

Every public method is safe to call from any thread.

* Reads take a shared lock; mutations take the authority lock followed by the exclusive state
  lock, in that order, for the shortest span that keeps state consistent.
* The persistence lock is always acquired last and is never held while a caller-supplied callback
  runs.
* No mutable internal reference escapes: queries return values.
* `mutate_node` and `mutate_edge` apply an edit to a copy of the stored record and
  re-index it atomically, so an in-place edit can never leave an index holding a stale membership.
* Snapshot construction computes content and digest under a shared lock and retries optimistically
  if a mutation lands in between.
* Fencing, retirement and reconciliation resolve deterministically under the exclusive lock.

Lock ordering, the absence of read-then-write re-acquisition on the same lock, and the
session/thread lifetime rules were audited by hand as well as by `ft_test_race`.

---

## 14. Determinism

Graph canonicalization, node and edge ordering, snapshots, diffs, explanations, reconciliation,
validation error ordering, serialization and digest computation are all deterministic. Insertion
order, container iteration order and process-local incarnation data never influence an externally
visible result, and the test suite checks each of those properties directly.

---

## 15. Validation truthfulness: REAL, SYNTHETIC, UNSUPPORTED

Every fact carries a `PublicationType` (`REAL`, `SYNTHETIC`,
`UNSUPPORTED`) and a `DiscoverySource`.

**REAL.** `discover_host_topology()` reads the actual development host on Windows: adapter
identities and interface GUIDs (`GetAdaptersAddresses`), friendly names, descriptions, MAC
addresses, MTU, operational state, assigned unicast addresses, next-hop gateway addresses and PnP
device instance identifiers matched through the network class registry key (`SetupAPI`).
Those are real host-local attachment facts, and `ft_test_host_discovery` publishes them
through the real reconciliation path. A host can prove its own attachment evidence. It cannot
prove the internal topology of a network it cannot see, and this adapter never invents such facts.

**SYNTHETIC.** `build_synthetic_topology` generates fifteen scenarios: single switch,
dual-switch redundancy, leaf-spine, multi-tier Clos, rack-to-leaf, multiple fabrics, multi-site,
logical overlay, physical plus logical, partitioned, device replacement, attachment move,
asymmetric publisher scopes, stale snapshot and large scale. All of them are tagged
`SYNTHETIC` with source `SYNTHETIC_GENERATOR`, none of them is physical evidence, and
the runtime carries that classification into provenance on every published relationship.

**UNSUPPORTED.** No real optical, InfiniBand, RoCE, NVLink-network, multi-site, SmartNIC or DPU
hardware topology is claimed: none of it is present in the development environment, and
`discover_host_topology()` reports the capabilities it cannot see rather than guessing.
External switch internals, cable peer identity, patch-panel mapping and remote fabric structure are
explicitly reported as unsupported.

---

## 16. Building

Requirements: CMake 3.25 or newer, a C++20 compiler and (for the runtime library and tools on
Windows) the Windows SDK. First-party code builds clean at `/W4 /WX` on MSVC; the equivalent
`-Wall -Wextra -Wpedantic` profile is used elsewhere.

    cmake -S . -B build -G "Visual Studio 17 2022" -A x64
    cmake --build build --config Release --parallel

Options:

| Option | Default | Effect |
| --- | --- | --- |
| `FABRIC_TOPOLOGY_BUILD_TESTS` | ON when top level | build the test suite |
| `FABRIC_TOPOLOGY_BUILD_TOOLS` | ON when top level | build `ftcli`, `ftcoordinator`, `ftpublisher` |
| `FABRIC_TOPOLOGY_BUILD_EXAMPLES` | ON when top level | build the examples |
| `FABRIC_TOPOLOGY_BUILD_BENCHMARKS` | ON when top level | build `ft_benchmarks` (never registered with CTest) |
| `FABRIC_TOPOLOGY_INSTALL_TOOLS` | ON | install the executables with the library |
| `FABRIC_TOPOLOGY_WARNINGS_AS_ERRORS` | ON | treat first-party warnings as errors |
| `FABRIC_TOPOLOGY_ENABLE_ASAN` | OFF | build with AddressSanitizer |

Debug works the same way:

    cmake --build build --config Debug --parallel

## 17. Testing

    ctest --test-dir build -C Release --output-on-failure

Seventeen test executables cover the core graph model and mutation semantics, structural
invariants, generation semantics, authority and fencing, reconciliation, determinism, traversal,
persistence, persistence corruption, resource bounds, seeded property testing, deterministic race
tests, adversarial hardening, the wire codec, real host discovery, the synthetic scenarios and the
multi-process distributed proofs.

There are no test timeouts. A hanging test is treated as a defect and diagnosed, never masked by a
watchdog.

## 18. Installing and consuming

    cmake --install build --config Release --prefix <prefix>

Downstream:

    find_package(FabricTopology CONFIG REQUIRED)
    target_link_libraries(app PRIVATE SummonSoftwareLabs::FabricTopology)

`SummonSoftwareLabs::FabricTopologyRuntime` is exported as well, for the socket, coordinator,
publisher-client, host-discovery and registry-seed adapters. The installed package contains the
headers, the static libraries, the CMake package configuration and version files and, when
`FABRIC_TOPOLOGY_INSTALL_TOOLS` is on, the executables. Nothing in the installed package
refers back to the source tree.

## 19. Examples

    cmake --build build --config Release --target ex_basic_physical_topology

| Example | Shows |
| --- | --- |
| `ex_basic_physical_topology` | fabric, switch, ports, NIC, cable, explanation, validation |
| `ex_leaf_spine_synthetic` | a synthetic leaf-spine publication |
| `ex_logical_and_physical` | a logical overlay backed by physical structure |
| `ex_stale_generation_rejection` | stale expectation rejection and idempotent replay |
| `ex_worker_fencing` | worker boot fencing, demotion and revalidation |
| `ex_snapshot_and_diff` | snapshot currentness, diff rendering and digests |
| `ex_device_replacement` | registry supersession and endpoint generation replacement |
| `ex_persistence_recovery` | durable image, recovery and conservative currentness |

All examples use only the installed public API.

## 20. Benchmarks

    cmake --build build --config Release --target ft_benchmarks
    build/Release/ft_benchmarks

Every figure is the wall-clock time of completed work: the operation has returned and its effect
is observable in authoritative state before the timer stops. Nothing measures an enqueue-only
path, and no target throughput is claimed. Measured on the development host (AMD Ryzen 7 9800X3D,
8 cores / 16 threads, MSVC 19.44, Release). Figures vary between runs; the table reflects
one complete run of the released binary:

| Operation | Count | Total (ms) | Per operation (ms) |
| --- | --- | --- | --- |
| `node_insert` | 10,000 | 26.190 | 0.002619 |
| `authoritative_snapshot_reconcile` (18,001 nodes / 89,990 edges) | 107,991 | 2,276.961 | 0.021085 |
| `edge_lookup` | 200,000 | 181.939 | 0.000910 |
| `neighbor_query` | 5,000 | 6.432 | 0.001286 |
| `snapshot_construction` | 5 | 2,144.337 | 428.867 |
| `deterministic_digest` | 5 | 1,177.808 | 235.562 |
| `diff_generation` | 107,991 | 102.518 | 0.000949 |
| `persistence_save` | 107,991 | 322.926 | 0.002990 |
| `persistence_load` | 107,991 | 1,547.899 | 0.014334 |
| `concurrent_readers` (8 threads) | 342,334 | 28.704 | 0.000084 |
| `full_validation` | 107,991 | 286.408 | 0.002652 |

Snapshot construction and digest computation are `O(N)` and dominated by the deterministic
SHA-256 digest of the canonical encoding: roughly 230 ms of the 429 ms snapshot figure is digest
time at this graph size. Both are reported exactly as measured, and neither is rounded up.

## 21. Tools

    ftcoordinator --port 0 --port-file port.txt --state state.ftstate --registry-seed seed.txt --domain dom-a --grant dom-a=authoritative
    ftpublisher   --write-seed seed.txt --scenario leaf_spine --scale 4 --tiers 2 --domain dom-a
    ftpublisher   --port <port> --publisher pub-a --domain dom-a --scenario leaf_spine --scale 4
    ftcli         query --port <port> --name edges
    ftcli         validate --state state.ftstate
    ftcli         scenarios
    ftcli         discover-host --out host.ftstate --seed-out host-seed.txt

`ftcli` inspects a durable image (nodes, edges, entities, neighbours, ancestors,
descendants, snapshot, digest, statistics, canonical rendering, persistence header) and is a thin
consumer of the public API: no topology logic lives in the CLI.

## 22. Limitations

* Fabric Topology does not model link health, reachability, capacity, congestion or forwarding. A
  `CURRENT` relationship means the structure is authoritative and the evidence behind it is
  live; it says nothing about operational condition.
* Structural traversal (`neighbors`, `ancestors`, `descendants`,
  `connected component`, `physical attachment chain`, `logical dependency chain`)
  is bounded graph inspection. It is not path computation and considers no cost, constraint or
  policy.
* Acyclicity maintenance costs `O(V + E_rel)` per insert for a relation class that requires
  it. There is no separate order-maintenance structure.
* The coordinator's transport is plain TCP with an optional shared-secret token and no per-scope
  authentication or encryption. Deployments that need those must terminate the protocol behind
  their own transport security.
* `InMemoryEntityDirectory` and the file-backed registry seed are adapters, not a Fabric
  Registry. A deployment links its own `IEntityDirectory`.
* Host discovery observes only the local host, and only on Windows in this build.
* Real optical, InfiniBand, RoCE, NVLink-network, multi-site, SmartNIC and DPU topology is
  unsupported and untested here because the hardware is not present.

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
