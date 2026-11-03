# UE 5.8 Chaos Physics: Integration Points for GPU FLIP / Rigid-Body Coupling

Source: `C:\Program Files\Epic Games\UE_5.8\Engine\Source` (5.8.4 launcher build). Paths below are relative to `Engine/Source/`.
Line numbers are from that build and will drift in later hotfixes.

Legend:
- **[Observed]**: read directly in the 5.8 headers or .cpp files.
- **[Inferred]**: my reasoning from the observed code, or general engine knowledge I did not re-check line by line.

Units: UE/Chaos use centimetres and kilograms, so force is kg·cm/s² and default gravity is −980 cm/s² [Inferred, standard]. Water density is 1000 kg/m³ = 0.001 kg/cm³.

---

## 1. Bodies: static, dynamic, kinematic (`FBodyInstance`)

Header: `Runtime/Engine/Classes/PhysicsEngine/BodyInstance.h`

### 1.1 Reading state [Observed]

| Line | Signature | Notes |
|---|---|---|
| 709 | `ENGINE_API UBodySetup* GetBodySetup() const;` | Simple collision lives in `UBodySetup::AggGeom`. |
| 711 | `ENGINE_API FPhysicsActorHandle GetPhysicsActor() const;` | Chaos `FSingleParticlePhysicsProxy*`. |
| 727 | `FBodyInstanceAsyncPhysicsTickHandle GetBodyInstanceAsyncPhysicsTickHandle() const` | Gives physics-thread access (see §3.4). |
| 572 | `FVector Scale3D;` (public) | Current physics scale. Needed to scale `AggGeom` elements. |
| 898–901 | `FVector GetCOMPosition() const { return GetMassSpaceToWorldSpace().GetLocation(); }` | World-space centre of mass. |
| 904 | `ENGINE_API FTransform GetMassSpaceToWorldSpace() const;` | Position is the world COM; rotation is the world orientation of the principal inertia axes. |
| 907 | `ENGINE_API FTransform GetMassSpaceLocal() const;` | Local COM offset. |
| 952 / 955 | `bool IsDynamic() const;` / `bool IsNonKinematic() const;` | |
| 958 | `ENGINE_API float GetBodyMass() const;` | kg |
| 960 | `ENGINE_API FBox GetBodyBounds() const;` | World AABB. Use it for GPU broad-phase culling. |
| 964 | `ENGINE_API FVector GetBodyInertiaTensor() const;` | Diagonal, **local mass space** (kg·cm²). |
| 989, 1626 | `bool IsInstanceSimulatingPhysics() const;` (inline: `ShouldInstanceSimulatingPhysics() && IsValidBodyInstance()`) | |
| 991 | `bool IsInstanceAwake() const;` | |
| 1184 | `bool IsValidBodyInstance() const;` | |
| 1187 | `ENGINE_API FTransform GetUnrealWorldTransform(bool bWithProjection = true, bool bForceGlobalPose = false) const;` | Body (actor) frame, not the COM frame. |
| 1193 | `ENGINE_API FTransform GetKinematicTarget() const;` | |
| 1205 | `ENGINE_API FVector GetUnrealWorldVelocity() const;` | cm/s |
| 1211 | `ENGINE_API FVector GetUnrealWorldAngularVelocityInRadians() const;` | rad/s, world space |
| 1217 | `ENGINE_API FVector GetUnrealWorldVelocityAtPoint(const FVector& Point) const;` | v + ω×(p−COM) |
| 625 | `ENGINE_API bool GetRigidBodyState(FRigidBodyState& OutState);` | Gets position, rotation, linear and angular velocity in one call. |

Every non-`_AssumesLocked` getter wraps its work in `FPhysicsCommand::ExecuteRead(GetPhysicsActor(), ...)`, which takes the scene read lock (`Runtime/Engine/Private/PhysicsEngine/BodyInstance.cpp:2749–2984`) [Observed]. If you read many bodies, take one lock yourself and call the `_AssumesLocked` variants (`GetUnrealWorldTransform_AssumesLocked` :1190, `GetUnrealWorldVelocity_AssumesLocked` :1208, and so on).

### 1.2 Applying forces [Observed]

| Line | Signature |
|---|---|
| 1001 | `ENGINE_API void AddForce(const FVector& Force, bool bAllowSubstepping = true, bool bAccelChange = false, const FAsyncPhysicsTimestamp TimeStamp = FAsyncPhysicsTimestamp(), APlayerController* PlayerController = nullptr);` |
| 1003 | `ENGINE_API void AddForceAtPosition(const FVector& Force, const FVector& Position, bool bAllowSubstepping = true, bool bIsLocalForce = false, const FAsyncPhysicsTimestamp TimeStamp = FAsyncPhysicsTimestamp(), APlayerController* PlayerController = nullptr);` |
| 1005 | `ENGINE_API void ClearForces(bool bAllowSubstepping = true);` |
| 1011 | `ENGINE_API void AddTorqueInRadians(const FVector& Torque, bool bAllowSubstepping = true, bool bAccelChange = false, const FAsyncPhysicsTimestamp TimeStamp = FAsyncPhysicsTimestamp(), APlayerController* PlayerController = nullptr);` |
| 1016 | `ENGINE_API void AddAngularImpulseInRadians(const FVector& Impulse, bool bVelChange, const FAsyncPhysicsTimestamp TimeStamp = FAsyncPhysicsTimestamp(), APlayerController* PlayerController = nullptr);` |
| 1019 | `ENGINE_API void AddImpulse(const FVector& Impulse, bool bVelChange, const FAsyncPhysicsTimestamp TimeStamp = FAsyncPhysicsTimestamp(), APlayerController* PlayerController = nullptr);` |
| 1022 | `ENGINE_API void AddImpulseAtPosition(const FVector& Impulse, const FVector& Position, const FAsyncPhysicsTimestamp TimeStamp = FAsyncPhysicsTimestamp(), APlayerController* PlayerController = nullptr);` |
| 1028 | `ENGINE_API void SetLinearVelocity(const FVector& NewVel, bool bAddToCurrent, bool bAutoWake = true);` |
| 1031 | `ENGINE_API void SetAngularVelocityInRadians(const FVector& NewAngVel, bool bAddToCurrent, bool bAutoWake = true);` |
| 999 | `ENGINE_API void AddCustomPhysics(FCalculateCustomPhysics& CalculateCustomPhysics);` (delegate: `Runtime/Engine/Public/PhysicsPublic.h:38` `DECLARE_DELEGATE_TwoParams(FCalculateCustomPhysics, float, FBodyInstance*)`) |

`FAsyncPhysicsTimestamp` (`Runtime/Engine/Classes/GameFramework/PlayerController.h:232`) is `{int32 ServerFrame = INDEX_NONE; int32 LocalFrame = INDEX_NONE;}`. Its `IsValid()` checks that both are set.

What the implementation does (`BodyInstance.cpp:3740–3791`, `Runtime/PhysicsCore/Private/ChaosEngineInterface.cpp:932–974, 1148–1205`) [Observed]:
- With the **default (invalid) timestamp**, `bIsInternal = false`. The command runs immediately on the calling (game) thread under `FPhysicsCommand::ExecuteWrite` and goes through the **game-thread API** (`FRigidBodyHandle_External`).
- With a **valid timestamp**, the command goes to `APlayerController::ExecuteAsyncPhysicsCommand` and runs later on the physics thread at that frame. This path is for networked physics prediction. It needs a PlayerController, so **do not use it for fluid forces**.
- The force is applied only if the body's object state is `Dynamic` or `Sleeping`. Calls on kinematic or static bodies are silently ignored (`IsBodyDynamic`, BodyInstance.cpp:3761).
- `bAllowSubstepping` is accepted and then **ignored** by Chaos (`FChaosStateOps::AddForce` never reads it).
- `bAccelChange = true` multiplies by mass, so the value you pass is treated as an acceleration.
- `AddForceAtPosition` computes `Torque = (Position − WorldCOM) × Force` itself and calls `AddForce` plus `AddTorque`. It **always wakes the body**: `SetObjectState(Dynamic)` runs even when the force is zero. `AddForce` wakes the body only if the force is non-zero. Practical rule: skip the call when the force is zero, or floating debris will never sleep.
- The game-thread particle stores the force as an acceleration: `SetAcceleration(F * InvM + Acceleration)` (`Runtime/Experimental/Chaos/Public/Chaos/ParticleHandle.h:3593–3597`). Calls in the same frame accumulate.
- `AddCustomPhysics` on Chaos: `FPhysScene_Chaos::AddCustomPhysics_AssumesLocked` just runs `CalculateCustomPhysics.ExecuteIfBound(MDeltaTime, BodyInstance)` once, immediately, on the calling thread (`PhysScene_Chaos.cpp:1665–1668`). The header comment says it runs once per substep; that is **not** what the code does. Don't rely on it for per-substep forces.

### 1.3 `UPrimitiveComponent` equivalents [Observed]
Header: `Runtime/Engine/Classes/Components/PrimitiveComponent.h`

| Line | Signature |
|---|---|
| 1661 | `virtual void SetSimulatePhysics(bool bSimulate);` |
| 1692 | `virtual void AddImpulse(FVector Impulse, FName BoneName = NAME_None, bool bVelChange = false);` |
| 1725 | `virtual void AddImpulseAtLocation(FVector Impulse, FVector Location, FName BoneName = NAME_None);` |
| 1759 | `virtual void AddForce(FVector Force, FName BoneName = NAME_None, bool bAccelChange = false);` |
| 1770 | `virtual void AddForceAtLocation(FVector Force, FVector Location, FName BoneName = NAME_None);` |
| 1781 | `virtual void AddForceAtLocationLocal(FVector Force, FVector Location, FName BoneName = NAME_None);` |
| 1802 | `virtual void AddTorqueInRadians(FVector Torque, FName BoneName = NAME_None, bool bAccelChange = false);` |
| 1832 | `FVector GetPhysicsLinearVelocity(FName BoneName = NAME_None) const;` |
| 1840 | `FVector GetPhysicsLinearVelocityAtPoint(FVector Point, FName BoneName = NAME_None) const;` |
| 1914 | `FVector GetPhysicsAngularVelocityInRadians(FName BoneName = NAME_None) const;` |
| 1922 | `FVector GetCenterOfMass(FName BoneName = NAME_None) const;` |
| 2311 | `virtual class UBodySetup* GetBodySetup() { return NULL; }` (overridden by StaticMesh, Shape and other components) |
| 2337 | `virtual FBodyInstance* GetBodyInstance(FName BoneName = NAME_None, bool bGetWelded = true, int32 Index = INDEX_NONE) const override;` |
| 2349 | `FBodyInstanceAsyncPhysicsTickHandle GetBodyInstanceAsyncPhysicsTickHandle(FName BoneName = NAME_None, bool bGetWelded = true, int32 Index = -1) const;` |
| 2644 | `virtual bool IsSimulatingPhysics(FName BoneName = NAME_None) const override;` |
| 2648 | `virtual FVector GetComponentVelocity() const override;` |
| 2861 | `virtual float GetMass() const;` |
| 2865 | `virtual FVector GetInertiaTensor(FName BoneName = NAME_None) const;` |
| 3248–3250 | `Chaos::FPhysicsObject* GetPhysicsObjectById(Chaos::FPhysicsObjectId) const; GetPhysicsObjectByName(const FName&) const; TArray<Chaos::FPhysicsObject*> GetAllPhysicsObjects() const;` (from `IPhysicsComponent`, `Runtime/Engine/Classes/Interfaces/IPhysicsComponent.h:20`) |

Pass `bGetWelded = true` (the default). A welded child shares its parent's simulated body, so forces must go to the weld root [Inferred, follows the API comment].

---

## 2. Collision shapes for building fluid obstacles

### 2.1 `UBodySetup` / `FKAggregateGeom` [Observed]
- `Runtime/Engine/Classes/PhysicsEngine/BodySetup.h:159`: `struct FKAggregateGeom AggGeom;`
- `Runtime/PhysicsCore/Public/BodySetupCore.h:27`: `FName BoneName;` (this is how physics-asset bodies map to bones)
- `Runtime/Engine/Classes/PhysicsEngine/AggregateGeom.h:23`: `struct FKAggregateGeom`
  - :28 `TArray<FKSphereElem> SphereElems;` :31 `BoxElems` :34 `SphylElems` :37 `ConvexElems` :40 `TaperedCapsuleElems` :43 `LevelSetElems` :46 `SkinnedLevelSetElems` :49 `MLLevelSetElems` :52 `SkinnedTriangleMeshElems`
  - :72 `int32 GetElementCount() const`; :570 `FBox CalcAABB(const FTransform& Transform) const;`; :586 `FVector::FReal GetScaledVolume(const FVector& Scale3D) const;`

Element fields:
| Element | Header:line | Fields | Local transform |
|---|---|---|---|
| `FKSphereElem` | SphereElem.h:14 | `FVector Center` (:25), `float Radius` (:29) | `GetTransform()` :66 = `FTransform(Center)` |
| `FKBoxElem` | BoxElem.h:15 | `Center` (:28), `FRotator Rotation` (:32), `float X, Y, Z` (:36–44) | `GetTransform()` :102. **X/Y/Z are full lengths, not half-extents.** The proof is `GetScaledVolume` = `X*Y*Z` (:117), so half-extent = X/2. |
| `FKSphylElem` (capsule) | SphylElem.h:15 | `Center`, `Rotation`, `Radius` (:36), `Length` (:40, line segment only; total length = Length + 2·Radius) | **Z axis is the capsule axis** (comment at :12). `GetScaledRadius` :119, `GetScaledHalfLength` :123 |
| `FKConvexElem` | ConvexElem.h:31 | `TArray<FVector> VertexData` (:37), `IndexData` (:40), `FBox ElemBox` (:44), private `FTransform Transform` (:49), `Chaos::FConvexPtr ChaosConvex` (:51), `GetChaosConvexMesh()` :103 | `GetTransform()` :127 |
| `FKShapeElem` base | ShapeElem.h:34 | `GetCollisionEnabled()` :111, `GetContributeToMass()` :102 | |

Each element type has `GetFinalScaled(const FVector& Scale3D, const FTransform& RelativeTM) const` (Sphere :95, Box :131, Sphyl :116). It returns a copy with non-uniform scale baked into the extents and transform: `GetTransform() * RelativeTM * ScaleTransform` (`BodySetup.cpp:2048–2065`).

**World-space primitive recipe** [Observed APIs, Inferred composition]:
```cpp
const FBodyInstance* BI = Prim->GetBodyInstance();            // weld root
const UBodySetup* BS    = BI->GetBodySetup();
const FTransform BodyTM = BI->GetUnrealWorldTransform();       // includes scale on the component
const FVector  Scale    = BodyTM.GetScale3D();                 // or BI->Scale3D
FTransform BodyNoScale  = BodyTM; BodyNoScale.SetScale3D(FVector::OneVector);
for (const FKSphylElem& E : BS->AggGeom.SphylElems)
{
    const FKSphylElem S = E.GetFinalScaled(Scale, FTransform::Identity);
    const FTransform  W = S.GetTransform() * BodyNoScale;      // capsule axis = W.GetUnitAxis(EAxis::Z)
    // upload {W.GetLocation(), W.GetRotation(), S.Radius, S.Length*0.5f}
}
```
For welded children, use the child's relative transform to the weld root as `RelativeTM` [Inferred]. Skip elements whose `GetCollisionEnabled()` is `NoCollision` or `QueryOnly` if only physics shapes should displace fluid.

### 2.2 Chaos implicit-object access (optional) [Observed]
- `Runtime/Experimental/Chaos/Public/PhysicsProxy/SingleParticlePhysicsProxy.h:289` `const FImplicitObjectRef GetGeometry() const;` and :294 `const FShapesArray& ShapesArray() const;`. Reach them via `BI->GetPhysicsActor()->GetGameThreadAPI()` (:78).
- `Runtime/Experimental/Chaos/Public/Chaos/ImplicitObject.h:110` `class FImplicitObject`: `AsA<T>()` :131, `GetType()` :199, `SignedDistance(const FVec3&)` :245, `PhiWithNormal(const FVec3&, FVec3&)` :251, `BoundingBox()` :266, `VisitLeafObjects(const FImplicitHierarchyVisitor&)` :447 (visitor signature :75: `void(const FImplicitObject*, const FRigidTransform3&, int32 Root, int32 ObjIdx, int32 LeafIdx)`).
- Type tags: `Chaos/ImplicitObjectType.h:8–33` (`Sphere, Box, Plane, Capsule, Transformed, Union, LevelSet, Convex, TriangleMesh, HeightField, TaperedCapsule, ...`).
- Use case: arbitrary convex or level-set colliders, which you can bake to a GPU SDF on the CPU with `SignedDistance`. For speed, prefer `AggGeom` analytic primitives.

---

## 3. Substeps, async physics, and physics-thread callbacks

### 3.1 `UPhysicsSettings` [Observed]
`Runtime/Engine/Classes/PhysicsEngine/PhysicsSettings.h`: class at :261, `static UPhysicsSettings* Get()` :419.
- :321 `float MaxPhysicsDeltaTime;`
- :325 `bool bSubstepping;` ("experimental")
- :329 `bool bSubsteppingAsync;`
- :333 `bool bTickPhysicsAsync;`
- :337 `float AsyncFixedTimeStepSize;`
- :341 `float MaxSubstepDeltaTime;`
- :345 `int32 MaxSubsteps;` (1..16)

How the solver steps (`Runtime/Experimental/Chaos/Private/Chaos/Framework/PhysicsSolverBase.cpp:581–760`) [Observed]:
- **Fixed-dt (async) mode**: time accumulates, and `NumSteps = floor(Accumulated / AsyncDt)`. NumSteps can be 0, 1, or several per game frame. Results are interpolated: `GetPhysicsResultsTime_External()` returns `ExternalTime − AsyncDt·AsyncMultiplier`, which is in the past (`PhysicsSolverBase.h:769–782`).
- **Substepping**: `NumSteps = ceil(dt / MaxDeltaTime)`, clamped to MaxSubsteps.
- Game-thread force handling across steps:
  - (a) If a game frame produces **no** physics step, its dirty dynamics carry over to the next frame. The next push divides them by the number of game frames involved (`ExternalSteps`, "we use this to average forces"; `FPBDRigidsSolver::PushPhysicsState`, `PBDRigidsSolver.cpp:1573–1600`, `DynamicsWeight = 1/NumExternalSteps`). Result: calling AddForce every frame gives the **average** force, not the sum.
  - (b) If a game frame produces **several** steps, accelerations (forces) are copied into every substep. Impulses are applied only in the first (`FPushPhysicsData::CopySubstepData`, `ChaosMarshallingManager.cpp:325–360`).
  - Net effect: a force you apply once per game frame acts as a constant force over that frame's simulated interval. That is the correct semantics for a fluid force sampled once per frame.

### 3.2 `AsyncPhysicsTickComponent` (exists in 5.8) [Observed]
- `Runtime/Engine/Classes/Components/ActorComponent.h:985`
  `virtual void AsyncPhysicsTickComponent(float DeltaTime, float SimTime) { ReceiveAsyncPhysicsTick(DeltaTime, SimTime); }`
- Enable flag: `uint8 bAsyncPhysicsTickEnabled : 1;` at :407. Setter `void SetAsyncPhysicsTickEnabled(bool bEnabled);` at :1435. `DeferRemoveAsyncPhysicsTick()` at :1439.
- Actor equivalent: `Runtime/Engine/Classes/GameFramework/Actor.h:2930` `virtual void AsyncPhysicsTickActor(float DeltaTime, float SimTime)`. Flag at :699.
- Mechanism: `FPhysScene_Chaos::RegisterAsyncPhysicsTickComponent` (`PhysScene_Chaos.h:142`) creates `FAsyncPhysicsTickCallback : TSimCallbackObject<FSimCallbackNoInput, FSimCallbackNoOutput, Presimulate | RunOnFrozenGameThread>` (`PhysScene_Chaos.cpp:146–240`). Its `OnPreSimulate_Internal` calls `AsyncPhysicsTickComponent(GetDeltaTime_Internal(), GetSimTime_Internal())` **once per physics step, on the game thread, while the physics thread is frozen**.
- The header warning (:403–406) says it "will cause both threads to halt and force a full sync". Inside it you may read and write physics-thread data. Use `GetBodyInstanceAsyncPhysicsTickHandle()->` (an `FRigidBodyHandle_Internal`) for physics-thread state. The engine's Blueprint helper is `UPhysicsThreadLibrary::AddForce(FBodyInstanceAsyncPhysicsTickHandle, FVector, bool bAccelChange)` (`Runtime/Engine/Public/Physics/Experimental/PhysicsThreadLibrary.h:32`).
- Use it when you need a force applied consistently per fixed physics step (for example the CPU-side implicit drag in PHYSICS_COUPLING.md). Cost: a game-thread/physics-thread sync point on every step.

### 3.3 Sim callback objects [Observed]
`Runtime/Experimental/Chaos/Public/Chaos/SimCallbackObject.h`
- :32 `enum class ESimCallbackOptions : uint32 { Presimulate=1<<0, MidPhaseModification, CCDModification, ContactModification=1<<3, StrainModification, ParticleRegister, ParticleUnregister, RunOnFrozenGameThread=1<<8, Rewind, PhysicsObjectUnregister, PreIntegrate=1<<11, PostIntegrate, PreSolve, PostSolve=1<<14, InjectInputsExternal, ProcessInputsExternal, ProcessInputsInternal }`
- :70 `class ISimCallbackObject`. Members: `GetSimTime_Internal()` :79, `GetDeltaTime_Internal()` :82, `GetSolver()` :206.
  - :322 `virtual void OnPreSimulate_Internal()`. Comment: "Called before simulation step (NOTE: not once per sub-step when sub-stepping is enabled)".
  - :332 `OnPreIntegrate_Internal()` and :342 `OnPostIntegrate_Internal()`: once per **sub-step**, opt-in via options.
  - :377 `OnContactModification_Internal(FCollisionContactModifier&)`; :398 `OnPostSolve_Internal()`.
- :572 `template <typename TInputType = FSimCallbackNoInput, typename TOutputType = FSimCallbackNoOutput, ESimCallbackOptions TOptions = ESimCallbackOptions::Presimulate> class TSimCallbackObject`
  - :597 `TInputType* GetProducerInputData_External()`: the game thread writes this frame's input.
  - :605 `const TInputType* GetConsumerInput_Internal() const`: the physics thread reads it, possibly several frames old.
  - :614 `TSimCallbackOutputHandle<TOutputType> PopOutputData_External()` (up to the results time); :643 `PopFutureOutputData_External()`.
  - :668 `TOutputType& GetProducerOutputData_Internal()`.
- `Chaos/SimCallbackInput.h:17` `struct FSimCallbackOutput { FReal InternalTime; }`, :33 `struct FSimCallbackInput`, :61/:66 `FSimCallbackNoInput/NoOutput`. Your input and output structs must provide `void Reset()`.

`Runtime/Experimental/Chaos/Public/Chaos/Framework/PhysicsSolverBase.h` [Observed]
- :346 `class FPhysicsSolverBase : public FPhysicsSolverEvents`
- :411–426 `template <typename TSimCallbackObjectType, typename... ConstructorArgTypes> TSimCallbackObjectType* CreateAndRegisterSimCallbackObject_External(ConstructorArgTypes&&... ConstructorArgs)`
- :453 `void UnregisterAndFreeSimCallbackObject_External(ISimCallbackObject* SimCallbackObject)`
- :469 `RegisterSimOneShotCallback(Lambda&&)`; :477 `EnqueueCommandImmediate(Lambda&&)` (game thread only)
- :638 `FReal GetAsyncDeltaTime() const`; :712 `bool IsUsingAsyncResults() const`; :717 `bool IsUsingFixedDt() const`
- :769 `FReal GetPhysicsResultsTime_External() const`. The 5.8 name is **`GetPhysicsResultsTime_External`**; there is no plain `GetPhysicsResultsTime`.
- :787 `const FReal GetSolverTime() const`
- Solver events (:283–296): `AddPreAdvanceCallback`, `AddPreBufferCallback`, `AddPostAdvanceCallback`, `AddTeardownCallback` (`FSolverPreAdvance/PostAdvance` = multicast `(Chaos::FReal Dt)`, :30/:32). The header says "Events are not threadsafe!" They fire on the physics thread.

Getting the solver: `UWorld::GetPhysicsScene()` (`Runtime/Engine/Classes/Engine/World.h:2891`) returns `FPhysScene*`, which is a typedef of `FPhysScene_Chaos` (`Runtime/PhysicsCore/Public/PhysicsInterfaceDeclaresCore.h:90`). `FChaosScene::GetSolver()` (`Runtime/PhysicsCore/Public/Chaos/ChaosScene.h:101`) returns `Chaos::FPhysicsSolver*`, which derives from `FPhysicsSolverBase`.

### 3.4 `FPhysScene_Chaos` delegates [Observed]
`Runtime/Engine/Public/Physics/Experimental/PhysScene_Chaos.h`
- :267–270
  `DECLARE_MULTICAST_DELEGATE_TwoParams(FOnPhysScenePreTick, FPhysScene_Chaos*, float);  FOnPhysScenePreTick OnPhysScenePreTick;`
  `DECLARE_MULTICAST_DELEGATE_TwoParams(FOnPhysSceneStep, FPhysScene_Chaos*, float);  FOnPhysSceneStep OnPhysSceneStep;`
  Both are broadcast back to back at the end of `FPhysScene_Chaos::OnStartFrame` (`PhysScene_Chaos.cpp:2272, 2313–2314`): **game thread, once per frame**, after replication and kinematic skeletal-mesh updates, before the solver is dispatched. In Chaos, `OnPhysSceneStep` is **not** a per-substep callback.
- `Runtime/PhysicsCore/Public/Chaos/ChaosScene.h:144–145` `DECLARE_MULTICAST_DELEGATE_OneParam(FOnPhysScenePostTick, FChaosScene*); FOnPhysScenePostTick OnPhysScenePostTick;`. Broadcast in `FChaosScene::EndFrame` (`ChaosScene.cpp:500/576`), game thread, after results sync.
- :231–238 the `_AssumesLocked` force helpers: `AddForce_AssumesLocked(FBodyInstance*, const FVector&, bool bAllowSubstepping, bool bAccelChange)`, `AddForceAtPosition_AssumesLocked(...)`, `AddTorque_AssumesLocked(...)`, `SetKinematicTarget_AssumesLocked(FBodyInstance*, const FTransform& TargetTM, bool bAllowSubstepping)`.

`OnPhysScenePreTick` is a good game-thread hook for "apply the latest fluid forces right before the physics step is dispatched" [Inferred]. Applying from a `TG_PrePhysics` component tick is equivalent.

### 3.5 `FPhysicsObject` interface (newer API) [Observed]
- `Runtime/Engine/Classes/PhysicsEngine/PhysicsObjectExternalInterface.h:48–60`, `class FPhysicsObjectExternalInterface`:
  `static FLockedReadPhysicsObjectExternalInterface LockRead(FChaosScene*)` (:50), `LockRead(Chaos::FConstPhysicsObjectHandle)` (:51), `LockRead(TArrayView<const Chaos::FConstPhysicsObjectHandle>)` (:52), `LockWrite(FChaosScene*)` (:53), `LockWrite(TArrayView<const Chaos::FPhysicsObjectHandle>)` (:54), `GetRead_AssumesLocked()` / `GetWrite_AssumesLocked()` (:56–57). The returned RAII object holds an `FScopedSceneLock_Chaos`; use it through `->`.
- `Runtime/Experimental/Chaos/Public/Chaos/PhysicsObjectInterface.h`
  - Read (:54 `FReadPhysicsObjectInterface<Id>`): `GetTransform` :62, `GetX` :63, `GetCoM` :64, `GetWorldCoM` :65, `GetR` :66, `GetV` :67, `GetVAtPoint` :68, `GetW` :69, `GetAllThreadShapes` :81, `GetGeometry` :82, `VisitEveryShape` :85, `GetMass(TArrayView<...>)` :95, `GetWorldBounds` :97.
  - Write (:113): `AddForce(TArrayView<const FPhysicsObjectHandle>, const FVector& Force, bool bInvalidate)` :120, `AddTorque(..., const FVector& Torque, bool bInvalidate, bool bAccelChange = false)` :123, `SetLinearVelocity` :125, `SetAngularVelocityInRadians` :126, `WakeUp` :118.
  - **There is no AddForceAtPosition.** Compute `τ = (p − GetWorldCoM) × F` yourself.
  - `FPhysicsObjectInterface::CreateReadInterface<Id>()` / `CreateWriteInterface<Id>()` :208/:212 select `EThreadContext::External` or `Internal`.
- Use case: geometry collections and cluster unions (fractured debris), which have several physics objects and no simple `FBodyInstance` per piece. For ordinary static or skeletal meshes, `FBodyInstance` is simpler.

---

## 4. Thread safety

| Context | Valid calls | Notes |
|---|---|---|
| **Game thread** (component Tick, `OnPhysScenePreTick`, `OnPhysScenePostTick`) | All `FBodyInstance` and `UPrimitiveComponent` getters and force calls with the default timestamp. `FPhysicsObjectExternalInterface::LockRead/LockWrite`. `World->OverlapMulti*` / `Sweep*`. `CreateAndRegisterSimCallbackObject_External`. `GetProducerInputData_External`, `PopOutputData_External`. | [Observed] Getters and setters lock internally (`FPhysicsCommand::ExecuteRead/Write`). With async physics, game-thread state is the **interpolated** result at `GetPhysicsResultsTime_External()`, which lags the solver. |
| **Physics thread** (`OnPreSimulate_Internal`, `OnPreIntegrate_Internal`, solver events) | Only `FRigidBodyHandle_Internal` (`Proxy->GetPhysicsThreadAPI()`, `SingleParticlePhysicsProxy.h:89`), `GetConsumerInput_Internal()`, `GetProducerOutputData_Internal()`, `FGenericPhysicsInterface_Internal` queries. | [Observed] `VerifyContext()` (`SingleParticlePhysicsProxy.h:1108–1120`) checks that external and internal APIs aren't mixed across threads. **No UObject access.** |
| **Frozen game thread** (`AsyncPhysicsTickComponent`, `RunOnFrozenGameThread` callbacks) | Game-thread objects **and** `FRigidBodyHandle_Internal` (via `FBodyInstanceAsyncPhysicsTickHandle`). | [Observed] Header comment. It halts both threads. |
| **Render thread / RDG passes** | **None** of the Chaos APIs. | [Inferred] There is no lock contract with the render thread. Marshal body data into a render-thread-owned struct on the game thread, for example with `ENQUEUE_RENDER_COMMAND` or a scene-view-extension snapshot. |

How game-thread forces reach the solver [Observed, §1.2 and §3.1]:
1. `AddForce*` writes acceleration into the game-thread particle's dirty `Dynamics` property and marks the proxy dirty.
2. At `StartFrame`/`AdvanceAndDispatch_External`, `PushPhysicsState` copies dirty properties into an `FPushPhysicsData` packet. Dynamics are weighted by 1/number of game frames since the last push.
3. The physics thread consumes the packet before `AdvanceOneTimeStep`. Forces apply to every substep in that interval. Impulses apply once.

---

## 5. Collision queries [Observed]
`Runtime/Engine/Classes/Engine/World.h`
- :2161 `bool LineTraceSingleByChannel(FHitResult& OutHit, const FVector& Start, const FVector& End, ECollisionChannel TraceChannel, const FCollisionQueryParams& Params = ..., const FCollisionResponseParams& ResponseParam = ...) const;`
- :2286 `bool SweepSingleByChannel(FHitResult&, const FVector& Start, const FVector& End, const FQuat& Rot, ECollisionChannel, const FCollisionShape&, ...) const;`
- :2325 `bool SweepMultiByChannel(TArray<FHitResult>& OutHits, const FVector& Start, const FVector& End, const FQuat& Rot, ECollisionChannel TraceChannel, const FCollisionShape& CollisionShape, const FCollisionQueryParams& Params = ..., const FCollisionResponseParams& ResponseParam = ...) const;`
- :2418 `bool OverlapMultiByChannel(TArray<FOverlapResult>& OutOverlaps, const FVector& Pos, const FQuat& Rot, ECollisionChannel TraceChannel, const FCollisionShape& CollisionShape, const FCollisionQueryParams& Params = ..., const FCollisionResponseParams& ResponseParam = ...) const;`
- :2430 `OverlapMultiByObjectType(...)`; :2485 `ComponentOverlapMulti(...)`; :2588 `AsyncSweepByChannel(...)`; :2658 `AsyncOverlapByChannel(...)` (results arrive next frame).
- Physics thread: `Runtime/Engine/Public/Physics/GenericPhysicsInterface.h:110` `Chaos::Private::FGenericPhysicsInterface_Internal` provides `SpherecastMulti` :120, `GeomSweepMulti` :131, `RaycastMulti` :141. It is marked "INTERNAL USE ONLY" and the API may change.

Coupling use: once per frame, run `OverlapMultiByObjectType(PhysicsBody)` with an `FCollisionShape::MakeBox(DomainHalfExtent)` at the fluid domain to discover which bodies to register as fluid colliders. Then filter by `IsInstanceSimulatingPhysics` or kinematic state [Inferred].

---

## 6. Moving and deforming geometry

- **Kinematic bodies**: `FBodyInstance` has **no** `SetKinematicTarget`. Use `SetBodyTransform(const FTransform&, ETeleportType Teleport, bool bAutoWake = true)` (BodyInstance.h:1202). For a simulated-kinematic body with `ETeleportType::None`, it calls `FPhysScene_Chaos::SetKinematicTarget_AssumesLocked(this, NewTransform, true)` (`BodyInstance.cpp:2808–2814`). Otherwise it sets the global pose. Lower level: `FChaosEngineInterface::SetKinematicTarget_AssumesLocked(const FPhysicsActorHandle&, const FTransform&)` (`Runtime/PhysicsCore/Public/Chaos/ChaosEngineInterface.h:359`). On the physics thread, kinematic targets are interpolated across substeps (`PseudoFraction`, `PhysicsSolverBase.cpp:248–251`) [Observed]. Normally `UPrimitiveComponent::SetWorldTransform` and `MoveComponent` on a non-simulating component reach this path [Inferred].
- **Velocity of kinematic or animated colliders**: Chaos gets kinematic velocity from targets, but for components moved by teleport or animation the physics velocity can be zero or unreliable [Inferred]. Recommendation: compute **v and ω by finite difference of the transforms you upload** (previous frame vs. current). This also matches the fluid's own timestep.
- **Skeletal meshes / physics assets**: `USkeletalMeshComponent::Bodies` (`SkeletalMeshComponent.h:1582`, public) is `TArray<FBodyInstance*>`. Each body's `GetBodySetup()` is a `USkeletalBodySetup` (`SkeletalBodySetup.h:25`) with `BoneName` and `AggGeom`. Asset side: `UPhysicsAsset::SkeletalBodySetups` (`PhysicsAsset.h:213`), `FindBodyIndex(FName)` (:366). Body transforms: `BI->GetUnrealWorldTransform()` when bodies exist, else `USkinnedMeshComponent::GetBoneTransform(FName)` (`SkinnedMeshComponent.h:1516`). Kinematic bodies follow animation via `UpdateKinematicBonesToAnim` (:2478), with deferral handled in `FPhysScene_Chaos::OnStartFrame` (`UpdateKinematicsOnDeferredSkelMeshes`, `PhysScene_Chaos.cpp:2296–2311`). Physics assets are mostly capsules (Sphyl), with some boxes and spheres. They map directly to analytic GPU SDF primitives.
- **Deforming triangle meshes**: `FBodyInstance::UpdateTriMeshVertices(const TArray<FVector>&)` (BodyInstance.h:895) exists, but complex collision is the wrong source for fluid SDFs. Prefer simple primitives, or a GPU-voxelised SDF from render geometry (outside Chaos) [Inferred].

---

## 7. Engine limitations relevant to GPU-computed forces

1. **Chaos is CPU-only.** There is no GPU mirror of Chaos particle state and no RHI-accessible Chaos buffers [Inferred; nothing in `Runtime/Experimental/Chaos` touches the RHI]. Body state must be packed on the game thread and uploaded to the GPU every frame.
2. **Readback latency.** GPU results need `FRHIGPUBufferReadback` (`Runtime/RHI/Public/RHIGPUReadback.h:132`; ctor `FRHIGPUBufferReadback(FName)` :136; `EnqueueCopy` :138; `IsReady()` :33; `void* Lock(uint32 NumBytes)` :139; `Unlock()` :140). The RDG helper is `AddEnqueueCopyPass(FRDGBuilder&, FRHIGPUBufferReadback*, FRDGBufferRef, uint32 NumBytes)` (`Runtime/RenderCore/Public/RenderGraphUtils.h:847`). With the game→render→GPU pipeline, data is typically ready **2–3 game frames** after the game frame whose body state it used [Inferred]. Never block on readback.
3. **Async physics adds latency.** With `bTickPhysicsAsync`, game-thread body state is interpolated and lies about one `AsyncFixedTimeStepSize` in the past (`GetPhysicsResultsTime_External`). That adds more lag between the poses you upload and the true solver state.
4. **Force semantics.** Game-thread forces act as constant over the frame's physics interval (§3.1). There is no API to apply a different GPU force per substep without a physics-thread callback fed through `TSimCallbackObject` inputs.
5. **`bAllowSubstepping` is ignored**, and `AddCustomPhysics` is not per substep (§1.2). For per-step behaviour such as implicit drag against the current velocity, use a `TSimCallbackObject` with `PreIntegrate` or `AsyncPhysicsTickComponent`.
6. **Determinism / networking.** GPU float reductions (atomics) are non-deterministic and the readback timing varies. Fluid forces will break Chaos resimulation and networked prediction unless you quantise and replicate them [Inferred].
