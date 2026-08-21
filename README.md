# winshipping

C++ UE5 actor-position monitor and targeting prototype for the `win-gaming` Windows VM. It loads the installed `memflow-qemu` and `memflow-win32` plugins through memflow's official C++ ABI.

## Configure

The executable name is configured in `src/Config.hpp`:

```cpp
inline constexpr std::string_view TargetProcessName = "FortniteClient-Win64-Shipping.exe";
```

## Build

```sh
make
```

The first build fetches the official memflow `0.2.4` source and builds its C++ FFI archive. The application itself is C++17; Cargo is only used to build that upstream ABI library. At runtime, memflow discovers plugins through its standard per-user installation directory, `MEMFLOW_PLUGIN_PATH`, and the current working directory. For a nonstandard installation, provide one or more plugin directories through `MEMFLOW_PLUGIN_PATH`:

```sh
MEMFLOW_PLUGIN_PATH=/path/to/memflow/plugins ./build/winshipping --read-only
```

Elevated execution is also supported when `HOME` changes: after standard memflow discovery, the program checks the original user exposed by `sudo`/`pkexec`, the real and effective users, and the owners of the executable and its parent directories. Conventional system plugin directories and plugins placed beside the executable are also recognized. `MEMFLOW_PLUGIN_PATH` remains the deterministic override for unusual installations.

The QEMU connector must be able to read the VM process maps and memory. Run the binary with the same permissions used by the existing memflow tools:

```sh
./build/winshipping --smoothing 11
```

Pass `--vehicle-smoothing FLOAT` to use a separate normal-aim smoothing value while the local pawn has a non-null `APlayerPawn::CurrentVehicle`. For example, `--smoothing 11 --vehicle-smoothing 5` uses `11` on foot and `5` as either a driver or passenger. The option is disabled when omitted, so it adds no vehicle-state reads to the normal polling path.

Pass repeatable `--weapon-smoothing TYPE=FLOAT` options to override normal-aim smoothing for the weapon currently held. For example, `--weapon-smoothing rifle=7 --weapon-smoothing shotgun=3 --weapon-smoothing sniper=12` leaves every unspecified weapon on the ordinary on-foot or vehicle value. Supported types are `pistol`, `shotgun`, `rifle`, `smg`, `sniper`, `launcher`, `bow`, `minigun`, `melee`, `utility`, `unarmed`, and `unknown`; `other` is accepted as an alias for `unknown`. If the same type appears more than once, the last value wins. A weapon override takes precedence over `--vehicle-smoothing` because it is more specific. Detection and its extra reads are disabled unless at least one weapon override is configured.

Pass `--ignore-isabot` to include AI-controlled match participants in actor output, aim selection, and chams. Without it, every player state marked by `APlayerState::bIsABot` is filtered, including fully functional match bots rather than only creative-map dummy pawns.

Pass `--ignore-teams` to allow aim selection and chams to consider pawns on the local team. The local pawn itself remains excluded. Team filtering remains enabled by default.

Pass `--chams` to apply the engine's pawn custom-depth highlight to eligible player pawns. It enables `UPawnComponent_CustomDepth`, sets all three affiliation slots to the original constant profile `12`, and forces that stencil and custom-depth bit onto the pawn's primary mesh. Chams do not use the render-timestamp visibility test, have no distance limit, and use the same bot and team filters as aim. Original component and mesh data is cached once and restored when a pawn leaves the eligible set or the program exits. `--read-only` suppresses all highlight writes.

The five closest resolved pawns are printed by default, sorted by three-dimensional distance from the local pawn and excluding the local pawn itself. Pass `--print-actors` to expand the table to every resolved pawn.

## DTB handling

The attach path implements both local patches:

1. Mask non-address bits above the physical 52-bit process DTB/CR3 value, matching the installed patched tooling.
2. Validate that the corrected DTB translates the target image to an `MZ`/`PE` header and permits PEB module enumeration.
3. Try the last recovered DTB from `/tmp/winshipping-dtb-v1.cache` when the process identity, image base, and protected reported DTB still match. The cached value is never trusted directly: it must pass the same PE and PEB-module validation before use.
4. If validation fails, scan guest-physical pages for PML4 candidates, retain candidates that translate the image base to a valid PE, select the candidate that produces the target's PEB module list, and update the cache.

## Runtime performance

The monitor uses separate rates for input, world data, highlighting, vehicle state, weapon state, and terminal output. The idle loop polls one complete memflow keyboard snapshot every 8 ms rather than issuing one guest read per key. Player/world data refreshes every 16 ms, but stable `GEngine -> GameViewport` state is memoized and per-player fields are gathered with scatter reads. When configured, `CurrentVehicle` and `CurrentWeapon` refresh independently every 100 ms. Held-weapon detection performs the direct `CurrentWeapon -> WeaponData -> ItemName` reads only at that rate. The active aim loop runs every 4 ms and reuses the latest actor, vehicle, and weapon state; when no aim key is held, it performs no camera, visibility, or bone reads. A snapshot older than 100 ms is discarded and any active aim state is cleared rather than targeting stale objects.

Highlights are reconciled every 16 ms because the game can clear the primary mesh's custom-depth bit after it is written. Ground-loot actors refresh every two seconds; actor classification, successful and failed item definitions, normalized item names, pickup-effect rarity values, and negative non-pickup results are cached. Initial and expired ground-actor classification is limited to 32 actors per world refresh so thousands of unrelated virtual reads cannot form one guest-stalling burst. Newly loaded actors enter that bounded queue immediately, while negative entries are revalidated after 30 seconds and removed when their actor leaves the loaded levels. Rarity fields are added to the same small scatter batch only when `--rarity` is active.

In a live 16-player read-only comparison, the original 1 ms full-scan loop averaged about 2.5% of one host CPU after attachment. The optimized idle loop averaged about 0.2% under the same bounded measurement. A cold protected-DTB recovery still performs the physical scan once, but a subsequent validated cache hit reduced a four-second launch sample from 23% average CPU to 1%.

The actor scan remains read-only. Player states marked by `APlayerState::bIsABot` are discarded before pawn resolution, so bots never enter the printed snapshot or targeting candidates. Aim candidates are also rejected unless `UWorld::Seconds - USkinnedMeshComponent::LastRenderTime <= 0.06`; this visibility filter affects targeting only and does not hide entries from the actor-position printout. Holding the right mouse button (`VK_RBUTTON`) activates the targeting prototype, which acquires the nearest visible enemy within a 30-degree camera cone and retains that pawn until right-click is released. Through 50 meters, bone resolution requests head index `110`; beyond 50 meters it requests torso index `3`. The requested index is tried directly and validated relative to the skeletal-mesh component rather than the pawn root, with geometric scanning used only when that transform is unavailable. Resolution prefers the array at `Mesh + 0x660` and falls back to `Mesh + 0x670`; failure skips the pawn rather than using its capsule. The target delta is calculated relative to the decoded camera, divided by the effective smoothing value, and written through `APlayerController::RotationInput`. A configured held-weapon override is selected first, then the vehicle value, then ordinary smoothing. `RotationInput` is cleared while idle, on shutdown, and when the controller changes. Every smoothing value must be at least `1`; ordinary smoothing defaults to `11`.

Holding `U` (`VK_U`) activates llama targeting and takes priority over right-click targeting. It resolves `MapInfo::LlamaClass`, caches exact-class actors from the loaded level arrays, selects the llama nearest to the active camera, and retains it until `U` is released. Camera-relative selection remains accurate while riding the battlebus, where the pawn root may not follow the aircraft. This route intentionally ignores mesh visibility and the normal camera-cone limit. Its smoothing is fixed at `1.0`, independent of `--smoothing`.

Pass `--item-name "NAME"` to enable ground-item targeting. The comparison is an ASCII case-insensitive substring, so `--item-name "stink rifle"` matches `Lawless Stink Rifle`. Add `--rarity RARITY` to set the minimum, case-insensitive rarity; accepted values are `Common`, `Uncommon`, `Rare`, `Epic`, `Legendary`, `Mythic`, `Transcendent`, and `Unattainable`. For example, `--item-name "shotgun" --rarity rare` includes rare, epic, legendary, mythic, transcendent, and unattainable shotguns while excluding common and uncommon ones. Rarity is resolved through the pickup effect's parent link and `APickupsParent::PickupRarityLevel`, rather than guessing from the item name or color. Loaded level actors are refreshed every two seconds, pickup metadata is cached, and the terminal reports the active name/minimum-rarity filter and current match count. Holding `Y` (`VK_Y`) selects the matching pickup nearest to the active camera and retains that actor until `Y` is released. This mode takes priority over llama and right-click targeting, ignores visibility and the normal camera-cone limit, and uses fixed smoothing `1.0`. Only pickup actors and effects replicated into the client's loaded levels can be found.

`PlayerArray` contains global player-state records, but the client does not necessarily instantiate a pawn for every remote state. The terminal distinguishes raw states, filtered bots, human states, and positioned pawns. A live 100-player-match check found 121 raw states, 95 bots, 26 human states, and only 2-4 locally instantiated human pawns; all unresolved humans failed specifically at `PawnPrivate`. Scanning roughly 1,490 persistent-level actors recovered none of the missing states, confirming that distant non-relevant pawns were absent from client memory rather than skipped by traversal.

Use `--clear-aim` to zero pending controller rotation input and exit without running the targeting loop.

Use `--read-only` during update validation to print actors, ground-item match counts, and the right-button, `U`, and `Y` states without writing any controller aim field.

Use `--bone-probe` for a one-shot, read-only comparison of the supplied primary and fallback bone-array pointers. Sample indices are treated only as structural probes and are not sourced from the potentially stale SDK.

The current Windows build 26200 kernel PDB places `_EPROCESS::SectionBaseAddress` at `0x2B0`. This is used as the fast path for obtaining the ASLR image base before the physical DTB scan when the protected reported DTB cannot read the PEB. The EPROCESS read is bounded to `0x400` bytes so an arbitrarily aligned allocation cannot fail from an unnecessary page crossing. Process selection requires an exact `FortniteClient` name so the similarly named EAC launcher is not selected. If the section-base field moves again, the program scans the bounded `_EPROCESS` prefix for aligned user-image candidates and validates ambiguous candidates through recovered DTBs instead of immediately falling back to the preferred image base.

## UE5 values

The core values below were refreshed for the current build. Values marked
`inferred` were not present in the supplied table and still need a fresh SDK or
live validation.

| Value | Offset |
|---|---:|
| direct `UWorld` global | `0x1B156318` (supplied, diagnostic only) |
| `GEngine` | `0x1B157C88` |
| `UGameEngine::GameViewport` | `0xB70` |
| `UGameViewportClient::World` | `0x78` (unverified carry-forward) |
| `UWorld::GameState` | `0x1C0` |
| `UWorld::OwningGameInstance` | `0x238` |
| world camera-location pointer | `0x168` |
| world encoded-camera-rotation pointer | `0x178` |
| `UWorld::Seconds` | `0x188` |
| `UWorld::Levels` | `0x1D8` |
| `AGameStateBase::PlayerArray` | `0x288` |
| `AGameStateAthena::MapInfo` | `0x2088` |
| `AAthenaMapInfo::LlamaClass` | `0x3D0` |
| `UGameInstance::LocalPlayers` | `0x38` |
| `UPlayer::PlayerController` | `0x30` |
| `APlayerController::AcknowledgedPawn` | `0x318` |
| `APlayerController::PlayerCameraManager` | `0x328` |
| `AController::ControlRotation` | `0x2E8` |
| `APlayerController::NetConnection` | `0x4A8` |
| `APlayerController::RotationInput` | `0x4B0` |
| `APlayerController::PlayerAimOffset` | `0x2310` |
| `APlayerController::WeaponRecoilOffset` | `0x2328` |
| `APlayerController::WeaponOffsetCorrection` | `0x2340` |
| `APlayerCameraManager::CameraCachePrivate` | `0x1590` (unverified carry-forward) |
| `APlayerState::PawnPrivate` | `0x2E8` |
| `APlayerState::bIsABot` | `0x27A`, bit 3 |
| `APlayerStateAthena::TeamIndex` | `0xF61` |
| `AActor::RootComponent` | `0x1B0` |
| live protected `UObject::ClassPrivate` | `0x20` |
| `ULevel::Actors` | `0x38` |
| `APickup::bPickedUp` | `0x28C`, bit 2 |
| `APickup::PrimaryPickupItemEntry` | `0x368` |
| `FItemEntry::ItemDefinition` | `0x10` |
| `UItemDefinitionBase::ItemName` | `0x38` |
| `APickupEffect::ParentPickupActor` | `0x290` (unverified carry-forward) |
| `APickupsParent::PickupRarityLevel` | `0x374` (unverified carry-forward) |
| `APawn::CurrentWeapon` | `0x9A0` |
| `APlayerPawn::CurrentVehicle` | `0x2B00` (unverified carry-forward) |
| `AWeapon::WeaponData` | `0x648` |
| SDK `FTextData` string pointer / length | `0x28` / `0x30` |
| legacy live `FTextData` fallback pointer / length | `0x20` / `0x28` |
| `APlayerPawn::Mesh` | `0x2F0` |
| `APlayerPawnAthena::CustomDepthComponent` | `0x4AF8` |
| `UPawnComponent_CustomDepth::bEnableRenderCustomDepth` | `0xD0`, bit 0 |
| `UPawnComponent_CustomDepth::DefaultHighlightingData` | `0xF0` |
| `UPrimitiveComponent::bRenderCustomDepth` | `0x291`, bit 2 |
| `UPrimitiveComponent::CustomDepthStencilValue` | `0x2A4` |
| mesh `ComponentToWorld` | `0x1E0` |
| primary bone-array pointer | `0x660` |
| cached bone-array pointer | `0x670` |
| `USceneComponent::RelativeLocation` | `0x140` |
| mesh `LastRenderTime` | `0x290` |

`FVector` is three 64-bit floating-point values in this SDK. The viewport is resolved once through `GEngine -> GameViewport`; each refresh reads its current `World`, then follows `GameState -> PlayerArray -> PawnPrivate -> RootComponent -> RelativeLocation`. The obsolete direct-GWorld fallback was deliberately removed because its old RVA can resolve unrelated memory after an update. The terminal is rewritten with the available pawn positions.

The preferred live camera path uses the pointers at `UWorld + 0x168` and `UWorld + 0x178` for location and encoded rotation. Encoded rotation values are read at `0x0`, `0x20`, and `0x1D0`, then decoded with `pitch = asin(c)` and `yaw = atan2(-a, b)`. The ordinary camera-cache/controller fields remain unverified fallbacks.

A read-only ground-loot pass decoded 95 pickups through `APickup -> PrimaryPickupItemEntry -> ItemDefinition -> ItemName`, including an exact `Lawless Stink Rifle` actor and position. A second live pass correlated pickup-effect actors through `ParentPickupActor` and validated rarity values `0` through `3` against common ammo, uncommon, rare, and epic weapons. The current SDK places the final `FTextData` string pointer and length at `0x28/0x30`; the decoder tries that layout first and retains the previously live-validated `0x20/0x28` layout as a checked fallback for carried-over objects.

Held-weapon classification follows `APawn + 0x9A0 -> AWeapon + 0x648 -> UItemDefinitionBase::ItemName`. The readable item name is grouped into pistol, shotgun, rifle, SMG, sniper, launcher, bow, minigun, melee, or unknown categories. Although the SDK also defines a semantic `EWeaponType`, no current weapon or weapon-definition member stores that enum directly, so the program does not guess a nonexistent field offset.

The current custom-depth pointer was recovered from the supplied `ReviveFromDBNOTime` anchor. That field moved from `0x4988` to `0x4A78` (`+0xF0`); preserving the reflected `+0x80` spacing to `CustomDepthComponent` predicts `0x4AF8`. A read-only live pass then confirmed `Pawn + 0x4AF8` across 117 instantiated pawns: every pointer had the same component class, and the highlighting payload and matched-component array remained valid at component offsets `0xF0` and `0xF8`. The component's old plain `Outer == pawn` invariant no longer holds in this protected layout, so runtime validation now checks component class consistency instead. A live match exposed that the component enable flag is normally clear and that changing only `DefaultHighlightingData` no longer propagates profile 12 to the mesh. The working path now also forces `bEnableRenderCustomDepth`, the primary mesh stencil, and the mesh custom-depth bit; the latter is refreshed because the game may clear it during normal mesh updates.

## Bone validation

The bone resolver intentionally does not consume generated SDK class metadata. The supplied current offsets place the pose arrays at `Mesh + 0x660` and `Mesh + 0x670`. Each entry is expected to remain a `0x60`-byte `FTransform`, with `Mesh + 0x1E0` providing `ComponentToWorld`; live validation is still required before treating the carried-forward transform layout and bone indices as confirmed for this build. Resolution prefers `0x660` and uses `0x670` only as a fallback.

The probe also established geometric landmarks without assigning SDK names: index `0` was approximately 75 units below the capsule origin, index `3` approximately 20-25 units above it, and index `110` tracked the anatomical head across the sampled pawns. Runtime poses can move the mesh substantially away from the capsule origin, so head validation is anchored to `ComponentToWorld` and does not require the head to remain above the pawn root.
