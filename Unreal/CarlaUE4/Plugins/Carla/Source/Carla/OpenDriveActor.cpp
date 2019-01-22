// Copyright (c) 2017 Computer Vision Center (CVC) at the Universitat Autonoma
// de Barcelona (UAB).
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

#include "Carla.h"

#include "OpenDriveActor.h"

#include "Util/OpenDrive.h"

#include <compiler/disable-ue4-macros.h>
#include <carla/road/WaypointGenerator.h>
#include <carla/rpc/String.h>
#include <carla/geom/Math.h>
#include <carla/road/element/Waypoint.h>
#include <compiler/enable-ue4-macros.h>

static TArray<FVector> WaypointVector2FVectorArray(
    const std::vector<carla::road::element::Waypoint> &Waypoints)
{
  TArray<FVector> Positions;
  Positions.Reserve(Waypoints.size());
  for (int i = 0; i < Waypoints.size(); ++i)
  {
    Positions.Add(Waypoints[i].ComputeTransform().location);
  }
  return Positions;
}

AOpenDriveActor::AOpenDriveActor(const FObjectInitializer &ObjectInitializer)
  : Super(ObjectInitializer)
{
  PrimaryActorTick.bCanEverTick = false;

  // Structure to hold one-time initialization
  static struct FConstructorStatics
  {
    // A helper class object we use to find target UTexture2D object in resource
    // package
    ConstructorHelpers::FObjectFinderOptional<UTexture2D> TextureObject;
    FName Category;
    FText Name;
    FConstructorStatics()
    // Use helper class object to find the texture resource path
      : TextureObject(TEXT("/Carla/Icons/OpenDriveActorIcon")),
        Category(TEXT("OpenDriveActor")),
        Name(NSLOCTEXT("SpriteCategory", "OpenDriveActor", "OpenDriveActor"))
    {}
  } ConstructorStatics;

  // We need a scene component to attach Icon sprite
  USceneComponent *SceneComponent =
      ObjectInitializer.CreateDefaultSubobject<USceneComponent>(this, TEXT("SceneComp"));
  RootComponent = SceneComponent;
  RootComponent->Mobility = EComponentMobility::Static;

#if WITH_EDITORONLY_DATA
  SpriteComponent =
      ObjectInitializer.CreateEditorOnlyDefaultSubobject<UBillboardComponent>(this, TEXT("Sprite"));
  if (SpriteComponent)
  {
    // Get the sprite texture from helper class object
    SpriteComponent->Sprite = ConstructorStatics.TextureObject.Get();
    // Assign sprite category name
    SpriteComponent->SpriteInfo.Category = ConstructorStatics.Category;
    // Assign sprite display name
    SpriteComponent->SpriteInfo.DisplayName = ConstructorStatics.Name;
    // Attach sprite to scene component
    SpriteComponent->SetupAttachment(RootComponent);
    SpriteComponent->Mobility = EComponentMobility::Static;
    SpriteComponent->SetEditorScale(1.0f);
  }
#endif // WITH_EDITORONLY_DATA
}

#if WITH_EDITOR
void AOpenDriveActor::PostEditChangeProperty(struct FPropertyChangedEvent &Event)
{
  Super::PostEditChangeProperty(Event);

  const FName PropertyName = (Event.Property != NULL ? Event.Property->GetFName() : NAME_None);
  if (PropertyName == GET_MEMBER_NAME_CHECKED(AOpenDriveActor, bGenerateRoutes))
  {
    if (bGenerateRoutes)
    {
      bGenerateRoutes = false;

      RemoveRoutes(); // Avoid OpenDrive overlapping
      RemoveSpawners(); // Restart the spawners in case OpenDrive has changed
      BuildRoutes();

      if (bAddSpawners)
      {
        AddSpawners();
      }
      if (bShowDebug)
      {
        DebugRoutes();
      }
    }
  }
  if (PropertyName == GET_MEMBER_NAME_CHECKED(AOpenDriveActor, bRemoveRoutes))
  {
    if (bRemoveRoutes)
    {
      bRemoveRoutes = false;

      RemoveDebugRoutes();
      RemoveSpawners();
      RemoveRoutes();
    }
  }
  if (PropertyName == GET_MEMBER_NAME_CHECKED(AOpenDriveActor, bShowDebug))
  {
    if (bShowDebug)
    {
      DebugRoutes();
    }
    else
    {
      RemoveDebugRoutes();
    }
  }
  if (PropertyName == GET_MEMBER_NAME_CHECKED(AOpenDriveActor, bRemoveCurrentSpawners))
  {
    if (bRemoveCurrentSpawners)
    {
      bRemoveCurrentSpawners = false;

      RemoveSpawners();
    }
  }
}
#endif // WITH_EDITOR

void AOpenDriveActor::BuildRoutes()
{
  using CarlaMath = carla::geom::Math;
  using IdType = carla::road::element::id_type;
  using Waypoint = carla::road::element::Waypoint;
  using WaypointGen = carla::road::WaypointGenerator;

  std::string ParseError;

  // As the OpenDrive file has the same name as level, build the path to the
  // xodr file using the lavel name and the game content directory.
  const FString XodrContent = FOpenDrive::Load(GetWorld()->GetMapName());

  auto map_ptr = carla::opendrive::OpenDrive::Load(
      TCHAR_TO_UTF8(*XodrContent),
      XmlInputType::CONTENT,
      &ParseError);

  if (ParseError.size())
  {
    UE_LOG(LogCarla, Error, TEXT("OpenDrive parsing error: '%s'."), *carla::rpc::ToFString(ParseError));
    return;
  }

  const auto &map = map_ptr->GetData();

  // List with waypoints, each one at the end of each lane of the map
  const std::vector<Waypoint> MapLaneBeginWaypoint =
      WaypointGen::GenerateLaneEnd(*map_ptr);

  for (auto &&EndLaneWaypoint : MapLaneBeginWaypoint)
  {
    std::vector<Waypoint> Successors = WaypointGen::GetSuccessors(EndLaneWaypoint);

    // generate the RoutePlanner
    ARoutePlanner *RoutePlanner = GetWorld()->SpawnActor<ARoutePlanner>();
    RoutePlanner->SetBoxExtent(FVector(70.f, 70.f, 50.f));
    RoutePlanner->SetActorRotation(EndLaneWaypoint.ComputeTransform().rotation);
    RoutePlanner->SetActorLocation(EndLaneWaypoint.ComputeTransform().location);

    // fill the RoutePlanner with all the needed roads
    for (auto &&Successor : Successors)
    {
      const IdType RoadId = Successor.GetRoadId();
      const float MaxDist = map.GetRoad(RoadId)->GetLength();

      std::vector<Waypoint> Waypoints;

      Waypoints.emplace_back(Successor);

      for (float Dist = RoadAccuracy; Dist < MaxDist; Dist += RoadAccuracy)
      {
        const auto NewWaypoint = WaypointGen::GetNext(Successor, Dist);

        assert(Dist < MaxDist);
        assert(NewWaypoint != nullptr);
        assert(NewWaypoint.size() == 1);
        assert(NewWaypoint[0] != nullptr);

        Waypoints.emplace_back(NewWaypoint[0]);
      }

      // merge with the first waypoint of the next lane if needed
      Waypoints.emplace_back(WaypointGen::GetNext(
          Successor, CarlaMath::clamp(MaxDist - 0.1f, 0.f, MaxDist))[0]);

      assert(Waypoints.size() >= 2);

      TArray<FVector> Positions = WaypointVector2FVectorArray(Waypoints);

      RoutePlanner->AddRoute(1.0f, Positions);
      RoutePlanners.Add(RoutePlanner);
    }
  }
}

void AOpenDriveActor::RemoveRoutes()
{
  const int rp_num = RoutePlanners.Num();
  for (int i = 0; i < rp_num; i++)
  {
    if (RoutePlanners[i] != nullptr)
    {
      RoutePlanners[i]->Destroy();
    }
  }
  RoutePlanners.Empty();
}

void AOpenDriveActor::DebugRoutes() const
{
  for (int i = 0; i < RoutePlanners.Num(); ++i)
  {
    if (RoutePlanners[i] != nullptr)
    {
      RoutePlanners[i]->DrawRoutes();
    }
  }
}

void AOpenDriveActor::RemoveDebugRoutes() const
{
#if WITH_EDITOR
  FlushPersistentDebugLines(GetWorld());
#endif   // WITH_EDITOR
}

void AOpenDriveActor::AddSpawners()
{
  for (int i = 0; i < RoutePlanners.Num(); ++i)
  {
    if (RoutePlanners[i] != nullptr)
    {
      FTransform Trans = RoutePlanners[i]->GetActorTransform();
      AVehicleSpawnPoint *Spawner = GetWorld()->SpawnActor<AVehicleSpawnPoint>();
      Spawner->SetActorRotation(Trans.GetRotation());
      Spawner->SetActorLocation(Trans.GetTranslation() + FVector(0.f, 0.f, SpawnersHeight));
      VehicleSpawners.Add(Spawner);
    }
  }
}

void AOpenDriveActor::RemoveSpawners()
{
  const int vs_num = VehicleSpawners.Num();
  for (int i = 0; i < vs_num; i++)
  {
    if (VehicleSpawners[i] != nullptr)
    {
      VehicleSpawners[i]->Destroy();
    }
  }
  VehicleSpawners.Empty();
}
