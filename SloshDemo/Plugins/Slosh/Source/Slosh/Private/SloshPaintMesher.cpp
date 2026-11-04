// Copyright notice: see Plugins/Slosh/NOTICE.md.

#include "SloshPaintMesher.h"
#include "Generators/MarchingCubes.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "DynamicMesh/MeshNormals.h"
#include "DynamicMesh/MeshIndexMappings.h"
#include "DynamicMeshEditor.h"
#include "Math/RandomStream.h"

using namespace UE::Geometry;

namespace Slosh::Paint
{
namespace
{
	struct FBlob
	{
		FVector3d Center;
		FVector3d SurfacePoint;   // on the painted surface (before Offset)
		FVector3d T, B, N;        // N = surface normal (ellipsoid short axis)
		double A = 1.0;           // tangential semi-axis
		double C = 1.0;           // normal semi-axis
		double Arc = 0.0;         // arc length along the stroke, cm
	};

	// Uniform bucket grid: every blob is inserted into all cells its (padded) bounds overlap, so one lookup
	// returns every blob that can influence a point.
	struct FBlobGrid
	{
		FVector3d Origin = FVector3d::ZeroVector;
		double CellSize = 1.0;
		FIntVector Dims = FIntVector(1, 1, 1);
		TArray<TArray<int32>> Cells;

		void Build(const TArray<FBlob>& Blobs, const FAxisAlignedBox3d& Bounds, double InCellSize, double Pad)
		{
			CellSize = FMath::Max(InCellSize, 1e-4);
			Origin = Bounds.Min;
			const FVector3d Extent = Bounds.Max - Bounds.Min;
			Dims = FIntVector(FMath::Max(1, int32(Extent.X / CellSize) + 1), FMath::Max(1, int32(Extent.Y / CellSize) + 1),
				FMath::Max(1, int32(Extent.Z / CellSize) + 1));
			Cells.SetNum(Dims.X * Dims.Y * Dims.Z);
			for (int32 Index = 0; Index < Blobs.Num(); ++Index)
			{
				const double R = FMath::Max(Blobs[Index].A, Blobs[Index].C) + Pad;
				const FIntVector Lo = CellOf(Blobs[Index].Center - FVector3d(R));
				const FIntVector Hi = CellOf(Blobs[Index].Center + FVector3d(R));
				for (int32 Z = Lo.Z; Z <= Hi.Z; ++Z)
				{
					for (int32 Y = Lo.Y; Y <= Hi.Y; ++Y)
					{
						for (int32 X = Lo.X; X <= Hi.X; ++X)
						{
							Cells[X + Dims.X * (Y + Dims.Y * Z)].Add(Index);
						}
					}
				}
			}
		}

		FIntVector CellOf(const FVector3d& P) const
		{
			const FVector3d L = (P - Origin) / CellSize;
			return FIntVector(FMath::Clamp(int32(FMath::FloorToDouble(L.X)), 0, Dims.X - 1),
				FMath::Clamp(int32(FMath::FloorToDouble(L.Y)), 0, Dims.Y - 1),
				FMath::Clamp(int32(FMath::FloorToDouble(L.Z)), 0, Dims.Z - 1));
		}

		const TArray<int32>& Find(const FVector3d& P) const
		{
			const FIntVector C = CellOf(P);
			return Cells[C.X + Dims.X * (C.Y + Dims.Y * C.Z)];
		}
	};

	// Ellipsoid distance bound (Quilez): exact on the axes, a good approximation elsewhere.
	double EllipsoidSDF(const FBlob& Blob, const FVector3d& P)
	{
		const FVector3d D = P - Blob.Center;
		const FVector3d L(D.Dot(Blob.T), D.Dot(Blob.B), D.Dot(Blob.N));
		const FVector3d R(Blob.A, Blob.A, Blob.C);
		const double K0 = FVector3d(L.X / R.X, L.Y / R.Y, L.Z / R.Z).Length();
		const double K1 = FVector3d(L.X / (R.X * R.X), L.Y / (R.Y * R.Y), L.Z / (R.Z * R.Z)).Length();
		return K1 > 1e-12 ? K0 * (K0 - 1.0) / K1 : -FMath::Min(Blob.A, Blob.C);
	}

	double SmoothMin(double A, double B, double K)
	{
		const double H = FMath::Max(K - FMath::Abs(A - B), 0.0) / K;
		return FMath::Min(A, B) - H * H * K * 0.25;
	}

	// Arc-length resampling with interpolated normals and pressure.
	void Resample(const TArray<FInputPoint>& In, double Spacing, TArray<FInputPoint>& Out, TArray<double>& OutArc, double& OutLength)
	{
		TArray<double> Cumulative;
		Cumulative.SetNum(In.Num());
		Cumulative[0] = 0.0;
		for (int32 I = 1; I < In.Num(); ++I)
		{
			Cumulative[I] = Cumulative[I - 1] + FVector3d::Distance(In[I].Position, In[I - 1].Position);
		}
		OutLength = Cumulative.Last();
		if (OutLength < 1e-6 || In.Num() < 2)
		{
			Out = { In[0] };
			OutArc = { 0.0 };
			OutLength = 0.0;
			return;
		}
		const int32 Count = FMath::Clamp(int32(FMath::CeilToDouble(OutLength / FMath::Max(Spacing, 1e-4))) + 1, 2, 20000);
		Out.SetNum(Count);
		OutArc.SetNum(Count);
		int32 Segment = 0;
		for (int32 K = 0; K < Count; ++K)
		{
			const double S = OutLength * double(K) / double(Count - 1);
			while (Segment < In.Num() - 2 && Cumulative[Segment + 1] < S)
			{
				++Segment;
			}
			const double SegLen = FMath::Max(Cumulative[Segment + 1] - Cumulative[Segment], 1e-9);
			const double Alpha = FMath::Clamp((S - Cumulative[Segment]) / SegLen, 0.0, 1.0);
			const FInputPoint& P0 = In[Segment];
			const FInputPoint& P1 = In[Segment + 1];
			Out[K].Position = FMath::Lerp(P0.Position, P1.Position, Alpha);
			Out[K].Normal = FMath::Lerp(P0.Normal, P1.Normal, Alpha).GetSafeNormal(1e-8, FVector3d::UnitZ());
			Out[K].Pressure = FMath::Lerp(P0.Pressure, P1.Pressure, Alpha);
			OutArc[K] = S;
		}
	}

	void SetPerVertexUV(FDynamicMesh3& Mesh, FDynamicMeshUVOverlay* Overlay, TFunctionRef<FVector2f(int32)> Value)
	{
		Overlay->ClearElements();
		TArray<int32> ElementOf;
		ElementOf.Init(INDEX_NONE, Mesh.MaxVertexID());
		for (int32 Vid : Mesh.VertexIndicesItr())
		{
			ElementOf[Vid] = Overlay->AppendElement(Value(Vid));
		}
		for (int32 Tid : Mesh.TriangleIndicesItr())
		{
			const FIndex3i Tri = Mesh.GetTriangle(Tid);
			Overlay->SetTriangle(Tid, FIndex3i(ElementOf[Tri.A], ElementOf[Tri.B], ElementOf[Tri.C]));
		}
	}

	uint32 HashFloat(uint32 H, float V)
	{
		return HashCombineFast(H, GetTypeHash(V));
	}
}

uint32 FPresetParams::Hash() const
{
	uint32 H = 0x51A5u;
	for (float V : { UnitCm, Scale, Thickness, Roughness, Flatness, Offset, Fading, MaterialUVScale, DropLength, DropSize,
		DropExponent, DropMove, DropSpeed })
	{
		H = HashFloat(H, V);
	}
	for (int32 V : { Quality, SmoothIterations, int32(DropMode), int32(bDropMoveRandom), DropMoveSeed, int32(bDropSpeedRandom), DropSpeedSeed })
	{
		H = HashCombineFast(H, GetTypeHash(V));
	}
	for (const FLayerParams& L : Layers)
	{
		H = HashCombineFast(H, GetTypeHash(int32(L.bEnabled) | (int32(L.bRods) << 1) | (int32(L.bInside) << 2)));
		for (float V : { L.Size, L.SizeRandom, L.Amount, L.Shift })
		{
			H = HashFloat(H, V);
		}
		H = HashCombineFast(H, GetTypeHash(L.Seed));
		H = HashCombineFast(H, GetTypeHash(L.MaterialSlot));
	}
	return H;
}

bool BuildStroke(const FStrokeJob& Job, const FBuildSettings& Settings, FStrokeResult& Out)
{
	const FPresetParams& P = Job.Preset;
	if (Job.Points.IsEmpty() || Settings.Reveal <= 0.0)
	{
		return false;
	}
	const double Unit = FMath::Max(double(P.UnitCm), 1e-4);
	const double Thickness = FMath::Clamp(double(P.Thickness), 0.1, 5.0);
	const double Flatness = FMath::Clamp(double(P.Flatness), 0.0, 1.0);

	// 1. Resample at the Fluid Painter spacing (Thickness x Unit).
	TArray<FInputPoint> Samples;
	TArray<double> Arc;
	double Length = 0.0;
	Resample(Job.Points, Thickness * Unit, Samples, Arc, Length);

	// 2. Drop window for travelling droplets (per-stroke random phase and speed, as Fluid Painter seeds by curve index).
	const double DropLen = FMath::Clamp(double(P.DropLength), 0.01, 1.0);
	const double DropSize = FMath::Max(double(P.DropSize), 1.0);
	double WindowStart = 0.0;
	double WindowEnd = 1.0;
	double FadeInOut = 1.0;
	if (P.DropMode == 2)
	{
		FRandomStream MoveRng(P.DropMoveSeed * 7919 + Job.Seed);
		FRandomStream SpeedRng(P.DropSpeedSeed * 104729 + Job.Seed);
		const double Base = P.bDropMoveRandom ? double(MoveRng.FRand()) : 1.0;
		const double SpeedRand = P.bDropSpeedRandom ? double(SpeedRng.FRand()) : 1.0;
		const double Phase = FMath::Frac(Base / (DropLen + 1.0) + (double(P.DropMove) + Settings.DropTime) * double(P.DropSpeed) * SpeedRand);
		WindowEnd = Phase * (DropLen + 1.0);
		WindowStart = WindowEnd - DropLen;
		FadeInOut = FMath::Clamp(1.0 + WindowStart / DropLen, 0.0, 1.0) * FMath::Clamp(1.0 - (WindowEnd - 1.0) / DropLen, 0.0, 1.0);
	}

	// 3. Blobs: flattened ellipsoids on the surface, radius from pressure x roughness jitter x drop profile.
	//    Fluid Painter turns the ellipsoids into points and then a volume with a per-point radius of
	//    Unit x clamp(Thickness, 0.5, 5) / (1 + Flatness / 2), which inflates every ellipsoid by that radius (flat,
	//    sunk presets such as Glaze+ rely on it to reach above the surface).
	const double Inflate = Unit * FMath::Clamp(Thickness, 0.5, 5.0) / (1.0 + 0.5 * Flatness);
	FRandomStream Rng(Job.Seed * 7919 + 17);
	TArray<FBlob> Blobs;
	Blobs.Reserve(Samples.Num());
	for (int32 K = 0; K < Samples.Num(); ++K)
	{
		const double U = Length > 0.0 ? Arc[K] / Length : 0.0;
		const double Jitter = 1.0 + double(P.Roughness) * (double(Rng.FRand()) * 2.0 - 1.0) * 0.5;
		if (U > Settings.Reveal + 1e-9)
		{
			continue;
		}
		double Profile = 1.0;
		if (P.DropMode == 1)
		{
			const double Start = 1.0 - DropLen;
			if (U >= Start)
			{
				Profile = FMath::Max(1.0, 1.0 + (DropSize - 1.0) * FMath::Pow((U - Start) / DropLen, double(P.DropExponent)));
			}
		}
		else if (P.DropMode == 2)
		{
			if (U < WindowStart || U > WindowEnd)
			{
				continue;
			}
			Profile = DropSize * FMath::Pow((U - WindowStart) / DropLen, double(P.DropExponent)) * FadeInOut;
		}
		const double CurveRadius = FMath::Clamp(Samples[K].Pressure * Jitter, 0.1, 5.0);
		const double R = Unit * FMath::Clamp(Thickness * Profile * CurveRadius, 0.1, 5.0 * Thickness);

		FBlob Blob;
		Blob.N = Samples[K].Normal;
		const FVector3d Prev = Samples[FMath::Max(K - 1, 0)].Position;
		const FVector3d Next = Samples[FMath::Min(K + 1, Samples.Num() - 1)].Position;
		FVector3d T = Next - Prev;
		T -= Blob.N * T.Dot(Blob.N);
		if (!T.Normalize(1e-9))
		{
			T = FMath::Abs(Blob.N.Z) < 0.9 ? Blob.N.Cross(FVector3d::UnitZ()).GetSafeNormal() : Blob.N.Cross(FVector3d::UnitX()).GetSafeNormal();
		}
		Blob.T = T;
		Blob.B = Blob.N.Cross(T);
		Blob.SurfacePoint = Samples[K].Position;
		Blob.Center = Samples[K].Position + Blob.N * (double(P.Offset) * Unit);
		Blob.A = R + Inflate;
		Blob.C = R * (1.0 - 0.99 * Flatness) + Inflate;
		Blob.Arc = Arc[K];
		Blobs.Add(Blob);
	}
	if (Blobs.IsEmpty())
	{
		return false;
	}

	// 4. Voxel size: Fluid Painter's (Unit x Thickness / flatness / quality), refined; flattened blobs must stay resolvable.
	double MeanA = 0.0;
	double MaxR = 0.0;
	FAxisAlignedBox3d Bounds = FAxisAlignedBox3d::Empty();
	for (const FBlob& Blob : Blobs)
	{
		MeanA += Blob.A;
		MaxR = FMath::Max(MaxR, Blob.A);
	}
	MeanA /= Blobs.Num();
	double Voxel = Unit * FMath::Clamp(Thickness, 0.5, 5.0) / ((1.0 + 0.5 * Flatness) * (2.0 + FMath::Clamp(P.Quality, 0, 2)));
	Voxel = FMath::Min(Voxel, MeanA / (3.0 + FMath::Clamp(P.Quality, 0, 2))) / FMath::Max(Settings.QualityScale, 0.1);
	for (FBlob& Blob : Blobs)
	{
		Blob.C = FMath::Max(Blob.C, 0.75 * Voxel);
	}
	const double Blend = 0.35 * MeanA;   // smooth-union width (stands in for the points -> volume merge)
	for (const FBlob& Blob : Blobs)
	{
		const double R = FMath::Max(Blob.A, Blob.C) + Blend;
		Bounds.Contain(Blob.Center - FVector3d(R));
		Bounds.Contain(Blob.Center + FVector3d(R));
	}
	Bounds.Expand(2.0 * Voxel);
	const double Cells = (Bounds.Width() / Voxel) * (Bounds.Height() / Voxel) * (Bounds.Depth() / Voxel);
	if (Cells > double(Settings.MaxCellsPerStroke))
	{
		Voxel *= FMath::Pow(Cells / double(Settings.MaxCellsPerStroke), 1.0 / 3.0);
	}

	FBlobGrid Grid;
	Grid.Build(Blobs, Bounds, 2.0 * MaxR + Blend, Blend);
	const double Far = 2.0 * MaxR + Blend;

	FMarchingCubes MarchingCubes;
	MarchingCubes.Bounds = Bounds;
	MarchingCubes.CubeSize = Voxel;
	MarchingCubes.IsoValue = 0.0;
	MarchingCubes.bParallelCompute = true;
	MarchingCubes.Implicit = [&Grid, &Blobs, Far, Blend](TVector<double> Pos)
	{
		double D = Far;
		for (int32 Index : Grid.Find(Pos))
		{
			D = SmoothMin(D, EllipsoidSDF(Blobs[Index], Pos), Blend);
		}
		return D;
	};
	MarchingCubes.Generate();
	if (MarchingCubes.Triangles.IsEmpty())
	{
		return false;
	}

	FDynamicMesh3& Mesh = Out.Mesh;
	Mesh = FDynamicMesh3(&MarchingCubes);

	// Outward winding regardless of the generator's convention. Unreal is left-handed: GeometryCore's triangle normal
	// is (C - A) x (B - A), so an outward-wound closed mesh has a negative A . (B x C) sum.
	double SignedVolume = 0.0;
	for (int32 Tid : Mesh.TriangleIndicesItr())
	{
		FVector3d A, B, C;
		Mesh.GetTriVertices(Tid, A, B, C);
		SignedVolume += A.Dot(B.Cross(C));
	}
	if (SignedVolume > 0.0)
	{
		for (int32 Tid : Mesh.TriangleIndicesItr())
		{
			Mesh.ReverseTriOrientation(Tid);
		}
	}

	// 5. Smoothing (the Fluid Painter Smooth modifier). Taubin lambda/mu passes: plain Laplacian shrank thin beads to
	//    a fraction of their designed radius on the finer voxel grid (measured), Taubin keeps the volume.
	{
		TArray<FVector3d> Next;
		Next.SetNum(Mesh.MaxVertexID());
		auto Pass = [&Mesh, &Next](double Factor)
		{
			for (int32 Vid : Mesh.VertexIndicesItr())
			{
				FVector3d Sum = FVector3d::ZeroVector;
				int32 Count = 0;
				for (int32 Nb : Mesh.VtxVerticesItr(Vid))
				{
					Sum += Mesh.GetVertex(Nb);
					++Count;
				}
				const FVector3d Pos = Mesh.GetVertex(Vid);
				Next[Vid] = Count > 0 ? Pos + (Sum / Count - Pos) * Factor : Pos;
			}
			for (int32 Vid : Mesh.VertexIndicesItr())
			{
				Mesh.SetVertex(Vid, Next[Vid]);
			}
		};
		for (int32 It = 0; It < FMath::Clamp(P.SmoothIterations, 0, 64); ++It)
		{
			Pass(0.5);
			Pass(-0.53);
		}
	}

	// 6. Attributes: normals, UV0 (fading, arc), UV1/UV2 (object coordinates for procedural noise), material slot.
	Mesh.EnableAttributes();
	FMeshNormals::InitializeOverlayToPerVertexNormals(Mesh.Attributes()->PrimaryNormals(), false);
	Mesh.Attributes()->SetNumUVLayers(NumUVLayers);
	Mesh.Attributes()->EnableMaterialID();
	for (int32 Tid : Mesh.TriangleIndicesItr())
	{
		Mesh.Attributes()->GetMaterialID()->SetValue(Tid, Job.MaterialSlot);
	}

	TArray<int32> NearestBlob;
	NearestBlob.Init(0, Mesh.MaxVertexID());
	for (int32 Vid : Mesh.VertexIndicesItr())
	{
		const FVector3d V = Mesh.GetVertex(Vid);
		double Best = TNumericLimits<double>::Max();
		const TArray<int32>& Candidates = Grid.Find(V);
		auto Consider = [&](int32 Index)
		{
			const double D = FVector3d::DistSquared(V, Blobs[Index].Center);
			if (D < Best)
			{
				Best = D;
				NearestBlob[Vid] = Index;
			}
		};
		if (Candidates.IsEmpty())
		{
			for (int32 Index = 0; Index < Blobs.Num(); ++Index)
			{
				Consider(Index);
			}
		}
		else
		{
			for (int32 Index : Candidates)
			{
				Consider(Index);
			}
		}
	}
	// Effective Fluid Painter "Scale" (includes the component's UnitScale): material features and layer density follow it.
	const double EffectiveScale = Unit / 0.1;
	const double FadeDistance = FMath::Max(double(P.Fading) * Unit, 1e-4);
	const double ObjectScale = 0.01 * double(P.MaterialUVScale) / FMath::Max(EffectiveScale, 1e-3);   // cm -> m, / Scale
	SetPerVertexUV(Mesh, Mesh.Attributes()->GetUVLayer(0), [&](int32 Vid)
	{
		const FBlob& Blob = Blobs[NearestBlob[Vid]];
		const double Height = (Mesh.GetVertex(Vid) - Blob.SurfacePoint).Dot(Blob.N);
		return FVector2f(float(FMath::Clamp(Height / FadeDistance, 0.0, 1.0)), float(Blob.Arc / Unit));
	});
	SetPerVertexUV(Mesh, Mesh.Attributes()->GetUVLayer(1), [&](int32 Vid)
	{
		const FVector3d V = Mesh.GetVertex(Vid) * ObjectScale;
		return FVector2f(float(V.X), float(V.Y));
	});
	SetPerVertexUV(Mesh, Mesh.Attributes()->GetUVLayer(2), [&](int32 Vid)
	{
		return FVector2f(float(Mesh.GetVertex(Vid).Z * ObjectScale), 0.0f);
	});

	// 7. Layers: area-weighted scatter on the liquid surface (Fluid Painter density Amount / Scale^2 x 1e5 per m^2).
	TArray<double> Cdf;
	TArray<int32> CdfTris;
	double Area = 0.0;
	for (int32 Tid : Mesh.TriangleIndicesItr())
	{
		Area += Mesh.GetTriArea(Tid);
		Cdf.Add(Area);
		CdfTris.Add(Tid);
	}
	for (int32 LayerIndex = 0; LayerIndex < 2; ++LayerIndex)
	{
		const FLayerParams& L = P.Layers[LayerIndex];
		if (!L.bEnabled || L.MaterialSlot == INDEX_NONE || Area <= 0.0)
		{
			continue;
		}
		const double Density = double(L.Amount) / FMath::Max(EffectiveScale * EffectiveScale, 1e-6) * 1e5;   // per m^2
		const int32 Count = FMath::Clamp(int32(FMath::RoundToDouble(Density * Area * 1e-4)), 0, Settings.MaxLayerInstancesPerStroke);
		if (Count == 0)
		{
			continue;
		}
		FLayerInstances& Instances = Out.Layers.AddDefaulted_GetRef();
		Instances.MaterialSlot = L.MaterialSlot;
		Instances.bRods = L.bRods;
		Instances.Transforms.Reserve(Count);
		FRandomStream LayerRng(L.Seed * 131 + Job.Seed * 7919 + LayerIndex);
		const double Size = double(L.Size) * Unit;
		for (int32 I = 0; I < Count; ++I)
		{
			const double Pick = double(LayerRng.FRand()) * Area;
			const int32 Slot = FMath::Min(Algo::LowerBound(Cdf, Pick), Cdf.Num() - 1);
			FVector3d A, B, C;
			Mesh.GetTriVertices(CdfTris[Slot], A, B, C);
			double R1 = LayerRng.FRand();
			double R2 = LayerRng.FRand();
			if (R1 + R2 > 1.0)
			{
				R1 = 1.0 - R1;
				R2 = 1.0 - R2;
			}
			const FVector3d Position = A + (B - A) * R1 + (C - A) * R2;
			const FVector3d Normal = Mesh.GetTriNormal(CdfTris[Slot]);
			const double Offset = (L.bInside ? -Size : 0.0) + double(L.Shift) * Unit;
			const double ScaleRand = (1.0 - L.SizeRandom) + L.SizeRandom * LayerRng.FRandRange(0.1f, 1.0f);
			FQuat Rotation = FQuat::FindBetweenNormals(FVector3d::UnitZ(), Normal);
			if (L.bRods)
			{
				Rotation = Rotation * FQuat(FRotator(LayerRng.FRandRange(0.f, 180.f), LayerRng.FRandRange(0.f, 180.f), LayerRng.FRandRange(0.f, 180.f)));
			}
			const FVector3d Scale3 = L.bRods
				? FVector3d(0.3 * Size * ScaleRand, 0.3 * Size * ScaleRand, 2.0 * Size * ScaleRand)
				: FVector3d(Size * ScaleRand);
			Instances.Transforms.Emplace(Rotation, Position + Normal * Offset, Scale3);
		}
	}
	return true;
}

void AppendStrokeMesh(FDynamicMesh3& Target, const FDynamicMesh3& Source)
{
	if (Target.TriangleCount() == 0)
	{
		Target = Source;
		return;
	}
	FDynamicMeshEditor Editor(&Target);
	FMeshIndexMappings Mappings;
	Editor.AppendMesh(&Source, Mappings);
}
}
