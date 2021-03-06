#include <ParticlePluginPCH.h>

#include <Core/ResourceManager/Resource.h>
#include <Core/ResourceManager/ResourceManager.h>
#include <Core/World/World.h>
#include <Foundation/Threading/Lock.h>
#include <Foundation/Threading/TaskSystem.h>
#include <GameEngine/Interfaces/PhysicsWorldModule.h>
#include <Module/ParticleModule.h>
#include <ParticlePlugin/Components/ParticleComponent.h>
#include <ParticlePlugin/Components/ParticleFinisherComponent.h>
#include <ParticlePlugin/Effect/ParticleEffectInstance.h>
#include <ParticlePlugin/Resources/ParticleEffectResource.h>
#include <ParticlePlugin/Streams/ParticleStream.h>
#include <ParticlePlugin/WorldModule/ParticleWorldModule.h>
#include <RendererCore/Pipeline/ExtractedRenderData.h>
#include <RendererCore/RenderWorld/RenderWorld.h>

// clang-format off
EZ_IMPLEMENT_WORLD_MODULE(ezParticleWorldModule);
EZ_BEGIN_DYNAMIC_REFLECTED_TYPE(ezParticleWorldModule, 1, ezRTTINoAllocator)
EZ_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

ezParticleWorldModule::ezParticleWorldModule(ezWorld* pWorld)
  : ezWorldModule(pWorld)
{
  m_uiExtractedFrame = 0;
}

ezParticleWorldModule::~ezParticleWorldModule()
{
  ClearParticleStreamFactories();
}

void ezParticleWorldModule::Initialize()
{
  ConfigureParticleStreamFactories();

  {
    auto updateDesc = EZ_CREATE_MODULE_UPDATE_FUNCTION_DESC(ezParticleWorldModule::UpdateEffects, this);
    updateDesc.m_Phase = ezWorldModule::UpdateFunctionDesc::Phase::PreAsync;
    updateDesc.m_bOnlyUpdateWhenSimulating = true;
    updateDesc.m_fPriority = 1000.0f; // kick off particle tasks as early as possible

    RegisterUpdateFunction(updateDesc);
  }

  {
    auto finishDesc = EZ_CREATE_MODULE_UPDATE_FUNCTION_DESC(ezParticleWorldModule::EnsureUpdatesFinished, this);
    finishDesc.m_Phase = ezWorldModule::UpdateFunctionDesc::Phase::PostTransform;
    finishDesc.m_bOnlyUpdateWhenSimulating = true;
    finishDesc.m_fPriority = -1000.0f; // sync with particle tasks as late as possible

    // make sure this function is called AFTER the components have done their final transform update
    finishDesc.m_DependsOn.PushBack(ezMakeHashedString("ezParticleComponentManager::UpdateTransforms"));

    RegisterUpdateFunction(finishDesc);
  }

  ezResourceManager::GetResourceEvents().AddEventHandler(ezMakeDelegate(&ezParticleWorldModule::ResourceEventHandler, this));

  {
    ezHybridArray<const ezRTTI*, 32> types;
    ezRTTI::GetAllTypesDerivedFrom(ezGetStaticRTTI<ezParticleModule>(), types, false);

    for (const ezRTTI* pRtti : types)
    {
      if (pRtti->GetAllocator()->CanAllocate())
      {
        ezUniquePtr<ezParticleModule> pModule = pRtti->GetAllocator()->Allocate<ezParticleModule>();
        pModule->RequestRequiredWorldModulesForCache(this);
      }
    }
  }
}


void ezParticleWorldModule::Deinitialize()
{
  EZ_LOCK(m_Mutex);

  ezResourceManager::GetResourceEvents().RemoveEventHandler(ezMakeDelegate(&ezParticleWorldModule::ResourceEventHandler, this));

  WorldClear();
}

void ezParticleWorldModule::EnsureUpdatesFinished(const ezWorldModule::UpdateContext& context)
{
  // do NOT lock here, otherwise tasks cannot enter the lock
  ezTaskSystem::WaitForGroup(m_EffectUpdateTaskGroup);

  {
    EZ_LOCK(m_Mutex);

    for (ezUInt32 i = 0; i < m_NeedFinisherComponent.GetCount(); ++i)
    {
      CreateFinisherComponent(m_NeedFinisherComponent[i]);
    }

    m_NeedFinisherComponent.Clear();
  }
}

void ezParticleWorldModule::ExtractRenderData(const ezView& view, ezExtractedRenderData& extractedRenderData) const
{
  EZ_ASSERT_RELEASE(ezTaskSystem::IsTaskGroupFinished(m_EffectUpdateTaskGroup), "Particle Effect Update Task is not finished!");

  EZ_LOCK(m_Mutex);

  // increase frame count to identify which system has been updated in this frame already
  ++m_uiExtractedFrame;

  for (auto it = m_ActiveEffects.GetIterator(); it.IsValid(); ++it)
  {
    ezParticleEffectInstance* pEffect = it.Value();

    if (pEffect->IsSharedEffect())
    {
      const auto& shared = pEffect->GetAllSharedInstances();

      for (ezUInt32 shi = 0; shi < shared.GetCount(); ++shi)
      {
        ExtractEffectRenderData(pEffect, view, extractedRenderData, pEffect->GetTransform(shared[shi].m_pSharedInstanceOwner));
      }
    }
    else
    {
      ExtractEffectRenderData(pEffect, view, extractedRenderData, pEffect->GetTransform(nullptr));
    }
  }
}


void ezParticleWorldModule::ExtractEffectRenderData(
  const ezParticleEffectInstance* pEffect, const ezView& view, ezExtractedRenderData& extractedRenderData, const ezTransform& systemTransform) const
{
  if (!pEffect->IsVisible())
    return;

  {
    // we know that at this point no one will modify the transform, as all threaded updates have been waited for
    // this will move the latest transform into the variable that is read by the renderer and thus the shaders will get the latest value
    // which is especially useful for effects that use local space simulation
    // this fixes the 'lag one frame behind' issue
    // however, we can't swap m_uiDoubleBufferReadIdx and m_uiDoubleBufferWriteIdx here, as this function is called on demand,
    // ie. it may be called 0 to N times (per active view)

    ezParticleEffectInstance* pEffect0 = const_cast<ezParticleEffectInstance*>(pEffect);
    pEffect0->m_Transform[pEffect->m_uiDoubleBufferReadIdx] = pEffect->m_Transform[pEffect->m_uiDoubleBufferWriteIdx];
  }

  for (ezUInt32 i = 0; i < pEffect->GetParticleSystems().GetCount(); ++i)
  {
    const ezParticleSystemInstance* pSystem = pEffect->GetParticleSystems()[i];

    if (pSystem == nullptr)
      continue;

    if (!pSystem->HasActiveParticles() || !pSystem->IsVisible())
      continue;

    pSystem->ExtractSystemRenderData(view, extractedRenderData, systemTransform, m_uiExtractedFrame);
  }
}

void ezParticleWorldModule::ResourceEventHandler(const ezResourceEvent& e)
{
  if (e.m_Type == ezResourceEvent::Type::ResourceContentUnloading && e.m_pResource->GetDynamicRTTI()->IsDerivedFrom<ezParticleEffectResource>())
  {
    EZ_LOCK(m_Mutex);

    ezParticleEffectResourceHandle hResource((ezParticleEffectResource*)(e.m_pResource));

    const ezUInt32 numEffects = m_ParticleEffects.GetCount();
    for (ezUInt32 i = 0; i < numEffects; ++i)
    {
      if (m_ParticleEffects[i].GetResource() == hResource)
      {
        m_EffectsToReconfigure.PushBack(&m_ParticleEffects[i]);
      }
    }
  }
}

void ezParticleWorldModule::ConfigureParticleStreamFactories()
{
  ClearParticleStreamFactories();

  ezStringBuilder fullName;

  for (ezRTTI* pRtti = ezRTTI::GetFirstInstance(); pRtti != nullptr; pRtti = pRtti->GetNextInstance())
  {
    if (!pRtti->IsDerivedFrom<ezParticleStreamFactory>() || !pRtti->GetAllocator()->CanAllocate())
      continue;

    ezParticleStreamFactory* pFactory = pRtti->GetAllocator()->Allocate<ezParticleStreamFactory>();

    ezParticleStreamFactory::GetFullStreamName(pFactory->GetStreamName(), pFactory->GetStreamDataType(), fullName);

    m_StreamFactories[fullName] = pFactory;
  }
}

void ezParticleWorldModule::ClearParticleStreamFactories()
{
  for (auto it : m_StreamFactories)
  {
    it.Value()->GetDynamicRTTI()->GetAllocator()->Deallocate(it.Value());
  }

  m_StreamFactories.Clear();
}

ezParticleStream* ezParticleWorldModule::CreateStreamDefaultInitializer(ezParticleSystemInstance* pOwner, const char* szFullStreamName) const
{
  auto it = m_StreamFactories.Find(szFullStreamName);
  if (!it.IsValid())
    return nullptr;

  return it.Value()->CreateParticleStream(pOwner);
}

ezWorldModule* ezParticleWorldModule::GetCachedWorldModule(const ezRTTI* pRtti) const
{
  ezWorldModule* pModule = nullptr;
  m_WorldModuleCache.TryGetValue(pRtti, pModule);
  return pModule;
}

void ezParticleWorldModule::CacheWorldModule(const ezRTTI* pRtti)
{
  m_WorldModuleCache[pRtti] = GetWorld()->GetOrCreateModule(pRtti);
}

void ezParticleWorldModule::CreateFinisherComponent(ezParticleEffectInstance* pEffect)
{
  if (pEffect && !pEffect->IsSharedEffect())
  {
    pEffect->SetVisibleIf(nullptr);

    ezWorld* pWorld = GetWorld();

    const ezTransform transform = pEffect->GetTransform();

    ezGameObjectDesc go;
    go.m_LocalPosition = transform.m_vPosition;
    go.m_LocalRotation = transform.m_qRotation;
    go.m_LocalScaling = transform.m_vScale;
    // go.m_Tags = GetOwner()->GetTags(); // TODO: pass along tags -> needed for rendering filters

    ezGameObject* pFinisher;
    pWorld->CreateObject(go, pFinisher);

    ezParticleFinisherComponent* pFinisherComp;
    ezParticleFinisherComponent::CreateComponent(pFinisher, pFinisherComp);

    pFinisherComp->m_EffectController = ezParticleEffectController(this, pEffect->GetHandle());
    pFinisherComp->m_EffectController.SetTransform(transform, ezVec3::ZeroVector()); // clear the velocity
  }
}

void ezParticleWorldModule::WorldClear()
{
  // make sure no particle update task is still in the pipeline
  ezTaskSystem::WaitForGroup(m_EffectUpdateTaskGroup);

  EZ_LOCK(m_Mutex);

  m_FinishingEffects.Clear();
  m_NeedFinisherComponent.Clear();

  m_ActiveEffects.Clear();
  m_ParticleEffects.Clear();
  m_ParticleSystems.Clear();
  m_ParticleEffectsFreeList.Clear();
  m_ParticleSystemFreeList.Clear();
}

EZ_STATICLINK_FILE(ParticlePlugin, ParticlePlugin_WorldModule_ParticleWorldModule);
