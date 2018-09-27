// Copyright (c) Improbable Worlds Ltd, All Rights Reserved

#pragma once

#include "CoreMinimal.h"

#include "SpatialTypebindingManager.h"
#include "Utils/RepDataUtils.h"

#include <improbable/c_schema.h>
#include <improbable/c_worker.h>


#include "SpatialSender.generated.h"

DECLARE_LOG_CATEGORY_EXTERN(LogSpatialSender, Log, All);

class USpatialNetDriver;
class UWorkingSetManager;
class USpatialView;
class USpatialWorkerConnection;
class USpatialActorChannel;
class USpatialPackageMapClient;
class USpatialTypebindingManager;
class USpatialReceiver;

struct FPendingRPCParams
{
	FPendingRPCParams(UObject* InTargetObject, UFunction* InFunction, void* InParameters)
		: TargetObject(InTargetObject), Function(InFunction), Parameters(InParameters) {}

	UObject* TargetObject;
	UFunction* Function;
	void* Parameters;
};

// TODO: Clear TMap entries when USpatialActorChannel gets deleted - UNR:100
// care for actor getting deleted before actor channel
using FChannelObjectPair = TPair<TWeakObjectPtr<USpatialActorChannel>, TWeakObjectPtr<UObject>>;
using FOutgoingRPCMap = TMap<const UObject*, TArray<FPendingRPCParams>>;
using FUnresolvedEntry = TSharedPtr<TSet<const UObject*>>;
using FHandleToUnresolved = TMap<uint16, FUnresolvedEntry>;
using FChannelToHandleToUnresolved = TMap<FChannelObjectPair, FHandleToUnresolved>;
using FOutgoingRepUpdates = TMap<const UObject*, FChannelToHandleToUnresolved>;

UCLASS()
class SPATIALGDK_API USpatialSender : public UObject
{
	GENERATED_BODY()

public:
	void Init(USpatialNetDriver* NetDriver);

	// Actor Updates
	void SendComponentUpdates(UObject* Object, USpatialActorChannel* Channel, const FRepChangeState* RepChanges, const FHandoverChangeState* HandoverChanges);
	void SendComponentInterest(AActor* Actor, Worker_EntityId EntityId);
	void SendPositionUpdate(Worker_EntityId EntityId, const FVector& Location);
	void SendRotationUpdate(Worker_EntityId EntityId, const FRotator& Rotation);
	void SendRPC(UObject* TargetObject, UFunction* Function, void* Parameters, bool bOwnParameters);
	void SendCommandResponse(Worker_RequestId request_id, Worker_CommandResponse& Response);

	void SendReserveEntityIdRequest(USpatialActorChannel* Channel);
	Worker_RequestId SendReserveEntityIdsRequest(const uint32& NumOfEntities);
	void SendCreateWorkingSetParentEntity(const Schema_EntityId* EntityId, const FVector& Location, const uint32& WorkingSetSize, const Worker_EntityId* ParentEntityId);
	void SendCreateEntityRequest(USpatialActorChannel* Channel, const FString& PlayerWorkerId, const Worker_EntityId* WorkingSetParentId);
	void SendDeleteEntityRequest(Worker_EntityId EntityId);

	void ResolveOutgoingOperations(UObject* Object, bool bIsHandover);
	void ResolveOutgoingRPCs(UObject* Object);

	bool UpdateEntityACLs(AActor* Actor, Worker_EntityId EntityId);
private:
	// Actor Lifecycle
	Worker_RequestId CreateEntity(const FString& ClientWorkerId, const FString& Metadata, USpatialActorChannel* Channel, const Schema_EntityId* WorkingSetParentId);

	// Queuing
	void ResetOutgoingUpdate(USpatialActorChannel* DependentChannel, UObject* ReplicatedObject, int16 Handle, bool bIsHandover);
	void QueueOutgoingUpdate(USpatialActorChannel* DependentChannel, UObject* ReplicatedObject, int16 Handle, const TSet<const UObject*>& UnresolvedObjects, bool bIsHandover);
	void QueueOutgoingRPC(const UObject* UnresolvedObject, UObject* TargetObject, UFunction* Function, void* Parameters);

	// RPC Construction
	Worker_CommandRequest CreateRPCCommandRequest(UObject* TargetObject, UFunction* Function, void* Parameters, Worker_ComponentId ComponentId, Schema_FieldId CommandIndex, Worker_EntityId& OutEntityId, const UObject*& OutUnresolvedObject);
	Worker_ComponentUpdate CreateMulticastUpdate(UObject* TargetObject, UFunction* Function, void* Parameters, Worker_ComponentId ComponentId, Schema_FieldId EventIndex, Worker_EntityId& OutEntityId, const UObject*& OutUnresolvedObject);

	TArray<Worker_InterestOverride> CreateComponentInterest(AActor* Actor);

private:
	UPROPERTY()
	USpatialNetDriver* NetDriver;

	UPROPERTY()
	USpatialView* View;

	UPROPERTY()
	USpatialWorkerConnection* Connection;

	UPROPERTY()
	USpatialReceiver* Receiver;

	UPROPERTY()
	USpatialPackageMapClient* PackageMap;

	UPROPERTY()
	USpatialTypebindingManager* TypebindingManager;
	UWorkingSetManager* WorkingSetManager;

	FChannelToHandleToUnresolved RepPropertyToUnresolved;
	FOutgoingRepUpdates RepObjectToUnresolved;

	FChannelToHandleToUnresolved HandoverPropertyToUnresolved;
	FOutgoingRepUpdates HandoverObjectToUnresolved;

	FOutgoingRPCMap OutgoingRPCs;

	TMap<Worker_RequestId, USpatialActorChannel*> PendingActorRequests;
};