// Copyright (c) Improbable Worlds Ltd, All Rights Reserved

#include "SpatialSender.h"

#include "EngineClasses/SpatialActorChannel.h"
#include "EngineClasses/SpatialNetBitWriter.h"
#include "EngineClasses/SpatialNetConnection.h"
#include "EngineClasses/SpatialNetDriver.h"
#include "EngineClasses/SpatialPackageMapClient.h"
#include "Interop/Connection/SpatialWorkerConnection.h"
#include "Interop/SpatialReceiver.h"
#include "Interop/SpatialView.h"
#include "Schema/Rotation.h"
#include "Schema/StandardLibrary.h"
#include "Schema/UnrealMetadata.h"
#include "SpatialConstants.h"
#include "Utils/ComponentFactory.h"
#include "Utils/RepLayoutUtils.h"

DEFINE_LOG_CATEGORY(LogSpatialSender);

using namespace improbable;

void USpatialSender::Init(USpatialNetDriver* InNetDriver)
{
	NetDriver = InNetDriver;
	View = InNetDriver->View;
	Connection = InNetDriver->Connection;
	Receiver = InNetDriver->Receiver;
	PackageMap = InNetDriver->PackageMap;
	TypebindingManager = InNetDriver->TypebindingManager;
}

Worker_RequestId USpatialSender::CreateEntity(const FString& ClientWorkerId, const FString& EntityType, USpatialActorChannel* Channel, const Schema_EntityId* WorkingSetParentId = nullptr)
{
	AActor* Actor = Channel->Actor;

	WorkerAttributeSet ServerAttribute = { SpatialConstants::ServerWorkerType };
	WorkerAttributeSet ClientAttribute = { SpatialConstants::ClientWorkerType };
	WorkerAttributeSet OwningClientAttribute = { TEXT("workerId:") + ClientWorkerId };

	WorkerRequirementSet ServersOnly = { ServerAttribute };
	WorkerRequirementSet ClientsOnly = { ClientAttribute };
	WorkerRequirementSet OwningClientOnly = { OwningClientAttribute };

	WorkerRequirementSet AnyUnrealServerOrClient = { ServerAttribute, ClientAttribute };
	WorkerRequirementSet AnyUnrealServerOrOwningClient = { ServerAttribute, OwningClientAttribute };

	WorkerRequirementSet ReadAcl;
	if (Actor->GetClass()->HasAnySpatialClassFlags(SPATIALCLASS_ServerOnly))
	{
		ReadAcl = ServersOnly;
	}
	else if (Actor->IsA<APlayerController>())
	{
		ReadAcl = AnyUnrealServerOrOwningClient;
	}
	else
	{
		ReadAcl = AnyUnrealServerOrClient;
	}

	FClassInfo* Info = TypebindingManager->FindClassInfoByClass(Actor->GetClass());
	check(Info);

	WriteAclMap ComponentWriteAcl;
	ComponentWriteAcl.Add(SpatialConstants::POSITION_COMPONENT_ID, ServersOnly);
	ComponentWriteAcl.Add(SpatialConstants::ROTATION_COMPONENT_ID, ServersOnly);
	ComponentWriteAcl.Add(Info->SingleClientComponent, ServersOnly);
	ComponentWriteAcl.Add(Info->MultiClientComponent, ServersOnly);
	ComponentWriteAcl.Add(Info->HandoverComponent, ServersOnly);
	ComponentWriteAcl.Add(Info->RPCComponents[RPC_Client], OwningClientOnly);
	ComponentWriteAcl.Add(Info->RPCComponents[RPC_Server], ServersOnly);
	ComponentWriteAcl.Add(Info->RPCComponents[RPC_CrossServer], ServersOnly);
	ComponentWriteAcl.Add(Info->RPCComponents[RPC_NetMulticast], ServersOnly);

	for (UClass* SubobjectClass : Info->SubobjectClasses)
	{
		FClassInfo* ClassInfo = TypebindingManager->FindClassInfoByClass(SubobjectClass);
		check(ClassInfo);

		ComponentWriteAcl.Add(ClassInfo->SingleClientComponent, ServersOnly);
		ComponentWriteAcl.Add(ClassInfo->MultiClientComponent, ServersOnly);
		ComponentWriteAcl.Add(ClassInfo->HandoverComponent, ServersOnly);
		ComponentWriteAcl.Add(ClassInfo->RPCComponents[RPC_Client], OwningClientOnly);
		ComponentWriteAcl.Add(ClassInfo->RPCComponents[RPC_Server], ServersOnly);
		ComponentWriteAcl.Add(ClassInfo->RPCComponents[RPC_CrossServer], ServersOnly);
		ComponentWriteAcl.Add(ClassInfo->RPCComponents[RPC_NetMulticast], ServersOnly);
	}

	TArray<Worker_ComponentData> ComponentDatas;
	ComponentDatas.Add(improbable::Position(improbable::Coordinates::FromFVector(Channel->GetActorSpatialPosition(Actor))).CreatePositionData());
	ComponentDatas.Add(improbable::Metadata(EntityType).CreateMetadataData());
	ComponentDatas.Add(improbable::EntityAcl(ReadAcl, ComponentWriteAcl).CreateEntityAclData());
	ComponentDatas.Add(improbable::Persistence().CreatePersistenceData());
	ComponentDatas.Add(improbable::Rotation(Actor->GetActorRotation()).CreateRotationData());
	ComponentDatas.Add(improbable::UnrealMetadata({}, ClientWorkerId, improbable::CreateOffsetMapFromActor(Actor)).CreateUnrealMetadataData());

	FUnresolvedObjectsMap UnresolvedObjectsMap;
	FUnresolvedObjectsMap HandoverUnresolvedObjectsMap;
	ComponentFactory DataFactory(UnresolvedObjectsMap, HandoverUnresolvedObjectsMap, NetDriver);

	FRepChangeState InitialRepChanges = Channel->CreateInitialRepChangeState(Actor);
	FHandoverChangeState InitialHandoverChanges = Channel->CreateInitialHandoverChangeState(Info);

	TArray<Worker_ComponentData> DynamicComponentDatas = DataFactory.CreateComponentDatas(Actor, InitialRepChanges, InitialHandoverChanges);
	ComponentDatas.Append(DynamicComponentDatas);

	for (auto& HandleUnresolvedObjectsPair : UnresolvedObjectsMap)
	{
		QueueOutgoingUpdate(Channel, Actor, HandleUnresolvedObjectsPair.Key, HandleUnresolvedObjectsPair.Value, /* bIsHandover */ false);
	}

	for (auto& HandleUnresolvedObjectsPair : HandoverUnresolvedObjectsMap)
	{
		QueueOutgoingUpdate(Channel, Actor, HandleUnresolvedObjectsPair.Key, HandleUnresolvedObjectsPair.Value, /* bIsHandover */ true);
	}

	// Working set data
 	if (WorkingSetParentId)
 	{
		ComponentWriteAcl.Add(SpatialConstants::WORKING_SET_COMPONENT_ID, ServersOnly);
		Worker_ComponentData WorkingSet = {};
 		WorkingSet.component_id = SpatialConstants::WORKING_SET_COMPONENT_ID;
		WorkingSet.schema_type = Schema_CreateComponentData(SpatialConstants::WORKING_SET_COMPONENT_ID);
		Schema_Object* ComponentObject = Schema_GetComponentDataFields(WorkingSet.schema_type);
		Schema_AddEntityId(ComponentObject, 2, *WorkingSetParentId);
		ComponentDatas.Add(WorkingSet);
 	}

	ComponentDatas.Add(EntityAcl(ReadAcl, ComponentWriteAcl).CreateEntityAclData());

	for (int RPCType = 0; RPCType < RPC_Count; RPCType++)
	{
		ComponentDatas.Add(ComponentFactory::CreateEmptyComponentData(Info->RPCComponents[RPCType]));
	}

	TArray<UObject*> DefaultSubobjects;
	Actor->GetDefaultSubobjects(DefaultSubobjects);

	for (UClass* SubobjectClass : Info->SubobjectClasses)
	{
		FClassInfo* SubobjectInfo = TypebindingManager->FindClassInfoByClass(SubobjectClass);
		check(SubobjectInfo);

		UObject** FoundSubobject = DefaultSubobjects.FindByPredicate([SubobjectClass](const UObject* Obj)
		{
			return Obj->GetClass() == SubobjectClass;
		});
		check(FoundSubobject);
		UObject* Subobject = *FoundSubobject;

		FRepChangeState SubobjectRepChanges = Channel->CreateInitialRepChangeState(Subobject);
		FHandoverChangeState SubobjectHandoverChanges = Channel->CreateInitialHandoverChangeState(SubobjectInfo);

		// Reset unresolved objects so they can be filled again by DataFactory
		UnresolvedObjectsMap.Empty();
		HandoverUnresolvedObjectsMap.Empty();

		TArray<Worker_ComponentData> ActorSubobjectDatas = DataFactory.CreateComponentDatas(Subobject, SubobjectRepChanges, SubobjectHandoverChanges);
		ComponentDatas.Append(ActorSubobjectDatas);

		for (auto& HandleUnresolvedObjectsPair : UnresolvedObjectsMap)
		{
			QueueOutgoingUpdate(Channel, Subobject, HandleUnresolvedObjectsPair.Key, HandleUnresolvedObjectsPair.Value, /* bIsHandover */ false);
		}

		for (auto& HandleUnresolvedObjectsPair : HandoverUnresolvedObjectsMap)
		{
			QueueOutgoingUpdate(Channel, Subobject, HandleUnresolvedObjectsPair.Key, HandleUnresolvedObjectsPair.Value, /* bIsHandover */ true);
		}

		for (int RPCType = 0; RPCType < RPC_Count; RPCType++)
		{
			ComponentDatas.Add(ComponentFactory::CreateEmptyComponentData(SubobjectInfo->RPCComponents[RPCType]));
		}
	}

	Worker_EntityId EntityId = Channel->GetEntityId();
	Worker_RequestId CreateEntityRequestId = Connection->SendCreateEntityRequest(ComponentDatas.Num(), ComponentDatas.GetData(), &EntityId);
	PendingActorRequests.Add(CreateEntityRequestId, Channel);

	return CreateEntityRequestId;
}

void USpatialSender::SendComponentUpdates(UObject* Object, USpatialActorChannel* Channel, const FRepChangeState* RepChanges, const FHandoverChangeState* HandoverChanges)
{
	Worker_EntityId EntityId = Channel->GetEntityId();

	UE_LOG(LogSpatialSender, Verbose, TEXT("Sending component update (object: %s, entity: %lld)"), *Object->GetName(), EntityId);

	FUnresolvedObjectsMap UnresolvedObjectsMap;
	FUnresolvedObjectsMap HandoverUnresolvedObjectsMap;
	ComponentFactory UpdateFactory(UnresolvedObjectsMap, HandoverUnresolvedObjectsMap, NetDriver);

	TArray<Worker_ComponentUpdate> ComponentUpdates = UpdateFactory.CreateComponentUpdates(Object, RepChanges, HandoverChanges);

	if (RepChanges)
	{
		for (uint16 Handle : RepChanges->RepChanged)
		{
			if (Handle > 0)
			{
				ResetOutgoingUpdate(Channel, Object, Handle, /* bIsHandover */ false);

				if (TSet<const UObject*>* UnresolvedObjects = UnresolvedObjectsMap.Find(Handle))
				{
					QueueOutgoingUpdate(Channel, Object, Handle, *UnresolvedObjects, /* bIsHandover */ false);
				}
			}
		}
	}

	if (HandoverChanges)
	{
		for (uint16 Handle : *HandoverChanges)
		{
			ResetOutgoingUpdate(Channel, Object, Handle, /* bIsHandover */ true);

			if (TSet<const UObject*>* UnresolvedObjects = HandoverUnresolvedObjectsMap.Find(Handle))
			{
				QueueOutgoingUpdate(Channel, Object, Handle, *UnresolvedObjects, /* bIsHandover */ true);
			}
		}
	}

	for (Worker_ComponentUpdate& Update : ComponentUpdates)
	{
		Connection->SendComponentUpdate(EntityId, &Update);
	}
}


void FillComponentInterests(FClassInfo* Info, bool bNetOwned, TArray<Worker_InterestOverride>& ComponentInterest)
{
	Worker_InterestOverride SingleClientInterest = { Info->SingleClientComponent, bNetOwned };
	ComponentInterest.Add(SingleClientInterest);

	Worker_InterestOverride HandoverInterest = { Info->HandoverComponent, false };
	ComponentInterest.Add(HandoverInterest);
}

TArray<Worker_InterestOverride> USpatialSender::CreateComponentInterest(AActor* Actor)
{
	TArray<Worker_InterestOverride> ComponentInterest;

	// This effectively checks whether the actor is owned by our PlayerController
	bool bNetOwned = Actor->GetNetConnection() != nullptr;

	FClassInfo* ActorInfo = TypebindingManager->FindClassInfoByClass(Actor->GetClass());
	FillComponentInterests(ActorInfo, bNetOwned, ComponentInterest);

	for (UClass* SubobjectClass : ActorInfo->SubobjectClasses)
	{
		FClassInfo* SubobjectInfo = TypebindingManager->FindClassInfoByClass(SubobjectClass);
		FillComponentInterests(SubobjectInfo, bNetOwned, ComponentInterest);
	}

	return ComponentInterest;
}

void USpatialSender::SendComponentInterest(AActor* Actor, Worker_EntityId EntityId)
{
	check(!NetDriver->IsServer());

	NetDriver->Connection->SendComponentInterest(EntityId, CreateComponentInterest(Actor));
}

void USpatialSender::SendPositionUpdate(Worker_EntityId EntityId, const FVector& Location)
{
	Worker_ComponentUpdate Update = improbable::Position::CreatePositionUpdate(improbable::Coordinates::FromFVector(Location));
	Connection->SendComponentUpdate(EntityId, &Update);
}

void USpatialSender::SendRotationUpdate(Worker_EntityId EntityId, const FRotator& Rotation)
{
	Worker_ComponentUpdate Update = improbable::Rotation(Rotation).CreateRotationUpdate();
	Connection->SendComponentUpdate(EntityId, &Update);
}

void USpatialSender::SendRPC(UObject* TargetObject, UFunction* Function, void* Parameters, bool bOwnParameters)
{
	FClassInfo* Info = TypebindingManager->FindClassInfoByClass(TargetObject->GetClass());
	if (Info == nullptr)
	{
		return;
	}

	FRPCInfo* RPCInfo = Info->RPCInfoMap.Find(Function);
	check(RPCInfo);

	Worker_EntityId EntityId = 0;
	const UObject* UnresolvedObject = nullptr;

	switch (RPCInfo->Type)
	{
	case RPC_Client:
	case RPC_Server:
	case RPC_CrossServer:
	{
		Worker_CommandRequest CommandRequest = CreateRPCCommandRequest(TargetObject, Function, Parameters, Info->RPCComponents[RPCInfo->Type], RPCInfo->Index + 1, EntityId, UnresolvedObject);

		if (!UnresolvedObject)
		{
			check(EntityId > 0);
			Connection->SendCommandRequest(EntityId, &CommandRequest, RPCInfo->Index + 1); 
		}
		break;
	}
	case RPC_NetMulticast:
	{
		Worker_ComponentUpdate ComponentUpdate = CreateMulticastUpdate(TargetObject, Function, Parameters, Info->RPCComponents[RPCInfo->Type], RPCInfo->Index + 1, EntityId, UnresolvedObject);

		if (!UnresolvedObject)
		{
			check(EntityId > 0);
			Connection->SendComponentUpdate(EntityId, &ComponentUpdate);
		}
		break;
	}
	default:
		checkNoEntry();
		break;
	}

	if (UnresolvedObject)
	{
		void* NewParameters = Parameters;
		if (!bOwnParameters)
		{
			// Copy parameters
			NewParameters = new uint8[Function->ParmsSize];
			FMemory::Memzero(NewParameters, Function->ParmsSize); // Not sure if needed considering we're copying everything from Parameters

			for (TFieldIterator<UProperty> It(Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
			{
				It->CopyCompleteValue_InContainer(NewParameters, Parameters);
			}
		}

		QueueOutgoingRPC(UnresolvedObject, TargetObject, Function, NewParameters);
	}
	else if (bOwnParameters)
	{
		// Destroy parameters
		for (TFieldIterator<UProperty> It(Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
		{
			It->DestroyValue_InContainer(Parameters);
		}
		delete[] (uint8*)Parameters;
	}
}

void USpatialSender::SendReserveEntityIdRequest(USpatialActorChannel* Channel)
{
	UE_LOG(LogSpatialSender, Log, TEXT("Sending reserve entity Id request for %s"), *Channel->Actor->GetName());
	Worker_RequestId RequestId = Connection->SendReserveEntityIdRequest();
	Receiver->AddPendingActorRequest(RequestId, Channel);
}

Worker_RequestId USpatialSender::SendReserveEntityIdsRequest(const uint32 & NumOfEntities)
{
	UE_LOG(LogTemp, Log, TEXT("Sending reserve entity Ids request for %d channels"), NumOfEntities);
	return Connection->SendReserveEntityIdsRequest(NumOfEntities);
}

void USpatialSender::SendCreateEntityRequest(USpatialActorChannel* Channel, const FString& PlayerWorkerId, const Worker_EntityId* WorkingSetParentId = nullptr)
{
	UE_LOG(LogSpatialSender, Log, TEXT("Sending create entity request for %s"), *Channel->Actor->GetName());

	FSoftClassPath ActorClassPath(Channel->Actor->GetClass());

	Worker_RequestId RequestId = CreateEntity(PlayerWorkerId, ActorClassPath.ToString(), Channel, WorkingSetParentId);
	Receiver->AddPendingActorRequest(RequestId, Channel);
}

void USpatialSender::SendCreateWorkingSetParentEntity(const Schema_EntityId* EntityId, const FVector& Location, const uint32& WorkingSetSize, const Worker_EntityId* ParentEntityId)
{
	WorkerAttributeSet WorkerAttribute = { TEXT("UnrealWorker") };
	WorkerAttributeSet ClientAttribute = { TEXT("UnrealClient") };
	WorkerRequirementSet WorkersOnly = { WorkerAttribute };
	WorkerRequirementSet ClientsOnly = { ClientAttribute };

	WorkerRequirementSet AnyUnrealWorkerOrClient = { WorkerAttribute, ClientAttribute };

	Worker_Entity WorkingSetParentEntity;
	WorkingSetParentEntity.entity_id = *EntityId;

	TArray<Worker_ComponentData> Components;

	WriteAclMap ComponentWriteAcl;
	ComponentWriteAcl.Add(SpatialConstants::POSITION_COMPONENT_ID, AnyUnrealWorkerOrClient);
	ComponentWriteAcl.Add(SpatialConstants::METADATA_COMPONENT_ID, WorkersOnly);
	ComponentWriteAcl.Add(SpatialConstants::PERSISTENCE_COMPONENT_ID, WorkersOnly);
	ComponentWriteAcl.Add(SpatialConstants::UNREAL_METADATA_COMPONENT_ID, WorkersOnly);
	ComponentWriteAcl.Add(SpatialConstants::ENTITY_ACL_COMPONENT_ID, WorkersOnly);
	ComponentWriteAcl.Add(SpatialConstants::WORKING_SET_COMPONENT_ID, AnyUnrealWorkerOrClient);

	Components.Add(improbable::Position(Coordinates::FromFVector(Location)).CreatePositionData());
	Components.Add(improbable::Metadata(TEXT("WorkingSetParent")).CreateMetadataData());
	Components.Add(improbable::Persistence().CreatePersistenceData());
	Components.Add(improbable::UnrealMetadata().CreateUnrealMetadataData());
	Components.Add(improbable::EntityAcl(AnyUnrealWorkerOrClient, ComponentWriteAcl).CreateEntityAclData());

	//Add entity ids
	Worker_ComponentData Data;
	Data.component_id = SpatialConstants::WORKING_SET_COMPONENT_ID;
	Data.schema_type = Schema_CreateComponentData(SpatialConstants::WORKING_SET_COMPONENT_ID);
	Schema_Object* ComponentObject = Schema_GetComponentDataFields(Data.schema_type);
	Schema_AddEntityIdList(ComponentObject, 1, EntityId, WorkingSetSize);
	Components.Add(Data);

	Connection->SendCreateEntityRequest(Components.Num(), Components.GetData(), ParentEntityId);
}

void USpatialSender::SendDeleteEntityRequest(Worker_EntityId EntityId)
{
	Connection->SendDeleteEntityRequest(EntityId);
}

void USpatialSender::ResetOutgoingUpdate(USpatialActorChannel* DependentChannel, UObject* ReplicatedObject, int16 Handle, bool bIsHandover)
{
	check(DependentChannel);
	check(ReplicatedObject);
	const FChannelObjectPair ChannelObjectPair(DependentChannel, ReplicatedObject);

	// Choose the correct container based on whether it's handover or not
	FChannelToHandleToUnresolved& PropertyToUnresolved = bIsHandover ? HandoverPropertyToUnresolved : RepPropertyToUnresolved;
	FOutgoingRepUpdates& ObjectToUnresolved = bIsHandover ? HandoverObjectToUnresolved : RepObjectToUnresolved;

	FHandleToUnresolved* HandleToUnresolved = PropertyToUnresolved.Find(ChannelObjectPair);
	if (HandleToUnresolved == nullptr)
	{
		return;
	}

	FUnresolvedEntry* UnresolvedPtr = HandleToUnresolved->Find(Handle);
	if (UnresolvedPtr == nullptr)
	{
		return;
	}

	FUnresolvedEntry& Unresolved = *UnresolvedPtr;

	check(Unresolved.IsValid());

	UE_LOG(LogSpatialSender, Log, TEXT("Resetting pending outgoing array depending on channel: %s, object: %s, handle: %d."),
		*DependentChannel->GetName(), *ReplicatedObject->GetName(), Handle);

	for (const UObject* UnresolvedObject : *Unresolved)
	{
		FChannelToHandleToUnresolved& ChannelToUnresolved = ObjectToUnresolved.FindChecked(UnresolvedObject);
		FHandleToUnresolved& OtherHandleToUnresolved = ChannelToUnresolved.FindChecked(ChannelObjectPair);

		OtherHandleToUnresolved.Remove(Handle);
		if (OtherHandleToUnresolved.Num() == 0)
		{
			ChannelToUnresolved.Remove(ChannelObjectPair);
			if (ChannelToUnresolved.Num() == 0)
			{
				ObjectToUnresolved.Remove(UnresolvedObject);
			}
		}
	}

	HandleToUnresolved->Remove(Handle);
	if (HandleToUnresolved->Num() == 0)
	{
		PropertyToUnresolved.Remove(ChannelObjectPair);
	}
}

void USpatialSender::QueueOutgoingUpdate(USpatialActorChannel* DependentChannel, UObject* ReplicatedObject, int16 Handle, const TSet<const UObject*>& UnresolvedObjects, bool bIsHandover)
{
	check(DependentChannel);
	check(ReplicatedObject);
	FChannelObjectPair ChannelObjectPair(DependentChannel, ReplicatedObject);

	UE_LOG(LogSpatialSender, Log, TEXT("Added pending outgoing property: channel: %s, object: %s, handle: %d. Depending on objects:"),
		*DependentChannel->GetName(), *ReplicatedObject->GetName(), Handle);

	// Choose the correct container based on whether it's handover or not
	FChannelToHandleToUnresolved& PropertyToUnresolved = bIsHandover ? HandoverPropertyToUnresolved : RepPropertyToUnresolved;
	FOutgoingRepUpdates& ObjectToUnresolved = bIsHandover ? HandoverObjectToUnresolved : RepObjectToUnresolved;

	FUnresolvedEntry Unresolved = MakeShared<TSet<const UObject*>>();
	*Unresolved = UnresolvedObjects;

	FHandleToUnresolved& HandleToUnresolved = PropertyToUnresolved.FindOrAdd(ChannelObjectPair);
	check(!HandleToUnresolved.Find(Handle));
	HandleToUnresolved.Add(Handle, Unresolved);

	for (const UObject* UnresolvedObject : UnresolvedObjects)
	{
		FHandleToUnresolved& AnotherHandleToUnresolved = ObjectToUnresolved.FindOrAdd(UnresolvedObject).FindOrAdd(ChannelObjectPair);
		check(!AnotherHandleToUnresolved.Find(Handle));
		AnotherHandleToUnresolved.Add(Handle, Unresolved);

		// Following up on the previous log: listing the unresolved objects
		UE_LOG(LogSpatialSender, Log, TEXT("- %s"), *UnresolvedObject->GetName());
	}
}

void USpatialSender::QueueOutgoingRPC(const UObject* UnresolvedObject, UObject* TargetObject, UFunction* Function, void* Parameters)
{
	check(UnresolvedObject);
	UE_LOG(LogSpatialSender, Log, TEXT("Added pending outgoing RPC depending on object: %s, target: %s, function: %s"), *UnresolvedObject->GetName(), *TargetObject->GetName(), *Function->GetName());
	OutgoingRPCs.FindOrAdd(UnresolvedObject).Add(FPendingRPCParams(TargetObject, Function, Parameters));
}

Worker_CommandRequest USpatialSender::CreateRPCCommandRequest(UObject* TargetObject, UFunction* Function, void* Parameters, Worker_ComponentId ComponentId, Schema_FieldId CommandIndex, Worker_EntityId& OutEntityId, const UObject*& OutUnresolvedObject)
{
	Worker_CommandRequest CommandRequest = {};
	CommandRequest.component_id = ComponentId;
	CommandRequest.schema_type = Schema_CreateCommandRequest(ComponentId, CommandIndex);
	Schema_Object* RequestObject = Schema_GetCommandRequestObject(CommandRequest.schema_type);

	UnrealObjectRef TargetObjectRef(PackageMap->GetUnrealObjectRefFromNetGUID(PackageMap->GetNetGUIDFromObject(TargetObject)));
	if (TargetObjectRef == SpatialConstants::UNRESOLVED_OBJECT_REF)
	{
		OutUnresolvedObject = TargetObject;
		Schema_DestroyCommandRequest(CommandRequest.schema_type);
		return CommandRequest;
	}

	OutEntityId = TargetObjectRef.Entity;

	TSet<const UObject*> UnresolvedObjects;
	FSpatialNetBitWriter PayloadWriter(PackageMap, UnresolvedObjects);

	TSharedPtr<FRepLayout> RepLayout = NetDriver->GetFunctionRepLayout(Function);
	RepLayout_SendPropertiesForRPC(*RepLayout, PayloadWriter, Parameters);

	for (const UObject* Object : UnresolvedObjects)
	{
		// Take the first unresolved object
		OutUnresolvedObject = Object;
		Schema_DestroyCommandRequest(CommandRequest.schema_type);
		return CommandRequest;
	}

	AddPayloadToSchema(RequestObject, 1, PayloadWriter);

	return CommandRequest;
}

Worker_ComponentUpdate USpatialSender::CreateMulticastUpdate(UObject* TargetObject, UFunction* Function, void* Parameters, Worker_ComponentId ComponentId, Schema_FieldId EventIndex, Worker_EntityId& OutEntityId, const UObject*& OutUnresolvedObject)
{
	Worker_ComponentUpdate ComponentUpdate = {};

	ComponentUpdate.component_id = ComponentId;
	ComponentUpdate.schema_type = Schema_CreateComponentUpdate(ComponentId);
	Schema_Object* EventsObject = Schema_GetComponentUpdateEvents(ComponentUpdate.schema_type);
	Schema_Object* EventData = Schema_AddObject(EventsObject, EventIndex);

	UnrealObjectRef TargetObjectRef(PackageMap->GetUnrealObjectRefFromNetGUID(PackageMap->GetNetGUIDFromObject(TargetObject)));
	if (TargetObjectRef == SpatialConstants::UNRESOLVED_OBJECT_REF)
	{
		OutUnresolvedObject = TargetObject;
		Schema_DestroyComponentUpdate(ComponentUpdate.schema_type);
		return ComponentUpdate;
	}

	OutEntityId = TargetObjectRef.Entity;

	TSet<const UObject*> UnresolvedObjects;
	FSpatialNetBitWriter PayloadWriter(PackageMap, UnresolvedObjects);

	TSharedPtr<FRepLayout> RepLayout = NetDriver->GetFunctionRepLayout(Function);
	RepLayout_SendPropertiesForRPC(*RepLayout, PayloadWriter, Parameters);

	for (const UObject* Object : UnresolvedObjects)
	{
		// Take the first unresolved object
		OutUnresolvedObject = Object;
		Schema_DestroyComponentUpdate(ComponentUpdate.schema_type);
		return ComponentUpdate;
	}

	AddPayloadToSchema(EventData, 1, PayloadWriter);

	return ComponentUpdate;
}

void USpatialSender::SendCommandResponse(Worker_RequestId request_id, Worker_CommandResponse& Response)
{
	Connection->SendCommandResponse(request_id, &Response);
}

void USpatialSender::ResolveOutgoingOperations(UObject* Object, bool bIsHandover)
{
	// Choose the correct container based on whether it's handover or not
	FChannelToHandleToUnresolved& PropertyToUnresolved = bIsHandover ? HandoverPropertyToUnresolved : RepPropertyToUnresolved;
	FOutgoingRepUpdates& ObjectToUnresolved = bIsHandover ? HandoverObjectToUnresolved : RepObjectToUnresolved;

	FChannelToHandleToUnresolved* ChannelToUnresolved = ObjectToUnresolved.Find(Object);
	if (!ChannelToUnresolved)
	{
		return;
	}

	for (auto& ChannelProperties : *ChannelToUnresolved)
	{
		FChannelObjectPair& ChannelObjectPair = ChannelProperties.Key;
		if (!ChannelObjectPair.Key.IsValid() || !ChannelObjectPair.Value.IsValid())
		{
			continue;
		}

		USpatialActorChannel* DependentChannel = ChannelObjectPair.Key.Get();
		UObject* ReplicatingObject = ChannelObjectPair.Value.Get();
		FHandleToUnresolved& HandleToUnresolved = ChannelProperties.Value;

		TArray<uint16> PropertyHandles;

		for (auto& HandleUnresolvedPair : HandleToUnresolved)
		{
			uint16 Handle = HandleUnresolvedPair.Key;
			FUnresolvedEntry& Unresolved = HandleUnresolvedPair.Value;

			Unresolved->Remove(Object);
			if (Unresolved->Num() == 0)
			{
				PropertyHandles.Add(Handle);

				// Hack to figure out if this property is an array to add extra handles
				if (!bIsHandover && DependentChannel->IsDynamicArrayHandle(ReplicatingObject, Handle))
				{
					PropertyHandles.Add(0);
					PropertyHandles.Add(0);
				}

				FHandleToUnresolved& AnotherHandleToUnresolved = PropertyToUnresolved.FindChecked(ChannelObjectPair);
				AnotherHandleToUnresolved.Remove(Handle);
				if (AnotherHandleToUnresolved.Num() == 0)
				{
					PropertyToUnresolved.Remove(ChannelObjectPair);
				}
			}
		}

		if (PropertyHandles.Num() > 0)
		{
			if (bIsHandover)
			{
				SendComponentUpdates(ReplicatingObject, DependentChannel, nullptr, &PropertyHandles);
			}
			else
			{
				// End with zero to indicate the end of the list of handles.
				PropertyHandles.Add(0);
				FRepChangeState RepChangeState = { PropertyHandles, DependentChannel->GetObjectRepLayout(ReplicatingObject) };
				SendComponentUpdates(ReplicatingObject, DependentChannel, &RepChangeState, nullptr);
			}
		}
	}

	ObjectToUnresolved.Remove(Object);
}

void USpatialSender::ResolveOutgoingRPCs(UObject* Object)
{
	TArray<FPendingRPCParams>* RPCList = OutgoingRPCs.Find(Object);
	if (RPCList)
	{
		for (FPendingRPCParams& RPCParams : *RPCList)
		{
			// We can guarantee that SendRPC won't populate OutgoingRPCs[Object] whilst we're iterating through it,
			// because Object has been resolved when we call ResolveOutgoingRPCs.
			UE_LOG(LogSpatialSender, Log, TEXT("Resolving outgoing RPC depending on object: %s, target: %s, function: %s"), *Object->GetName(), *RPCParams.TargetObject->GetName(), *RPCParams.Function->GetName());
			SendRPC(RPCParams.TargetObject, RPCParams.Function, RPCParams.Parameters, true);
		}
		OutgoingRPCs.Remove(Object);
	}
}

bool USpatialSender::UpdateEntityACLs(AActor* Actor, Worker_EntityId EntityId)
{
	improbable::EntityAcl* EntityACL = View->GetEntityACL(EntityId);

	if (EntityACL == nullptr)
	{
		return false;
	}

	FClassInfo* Info = TypebindingManager->FindClassInfoByClass(Actor->GetClass());
	check(Info);

	FString PlayerWorkerId;
	if (Actor->GetNetConnection() != nullptr)
	{
		PlayerWorkerId = Actor->GetNetConnection()->PlayerController->PlayerState->UniqueId.ToString();
	}

	WorkerAttributeSet OwningClientAttribute = { TEXT("workerId:") + PlayerWorkerId };
	WorkerRequirementSet OwningClientOnly = { OwningClientAttribute };

	if (EntityACL->ComponentWriteAcl.Contains(Info->RPCComponents[RPC_Client]))
	{
		EntityACL->ComponentWriteAcl[Info->RPCComponents[RPC_Client]] = OwningClientOnly;
	}
	else
	{
		EntityACL->ComponentWriteAcl.Add(Info->RPCComponents[RPC_Client], OwningClientOnly);
	}

	Worker_ComponentUpdate Update = EntityACL->CreateEntityAclUpdate();

	Connection->SendComponentUpdate(EntityId, &Update);
	return true;
}