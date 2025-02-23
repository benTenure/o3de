
/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */

#include <Family/BlastFamilyImpl.h>

#include <AzCore/Interface/Interface.h>
#include <AzCore/Debug/Profiler.h>
#include <Blast/BlastSystemBus.h>
#include <Family/ActorTracker.h>
#include <Family/BlastFamily.h>
#include <NvBlastExtPxAsset.h>
#include <NvBlastExtPxManager.h>
#include <NvBlastTkActor.h>
#include <NvBlastTkAsset.h>
#include <NvBlastTkEvent.h>
#include <NvBlastTkFamily.h>
#include <NvBlastTkFramework.h>
#include <NvBlastTkGroup.h>
#include <NvBlastTkJoint.h>
#include <PhysX/MathConversion.h>
#include <numeric>

namespace Blast
{
    AZStd::unique_ptr<BlastFamily> BlastFamily::Create(const BlastFamilyDesc& desc)
    {
        return AZStd::make_unique<BlastFamilyImpl>(desc);
    }

    BlastFamilyImpl::BlastFamilyImpl(const BlastFamilyDesc& desc)
        : m_asset(desc.m_asset)
        , m_actorFactory(desc.m_actorFactory)
        , m_entityProvider(desc.m_entityProvider)
        , m_listener(desc.m_listener)
        , m_physicsMaterialId(desc.m_physicsMaterial)
        , m_blastMaterial(desc.m_blastMaterial)
        , m_actorConfiguration(desc.m_actorConfiguration)
        , m_isSpawned(false)
    {
        Nv::Blast::TkFramework* tkFramework = AZ::Interface<BlastSystemRequests>::Get()->GetTkFramework();
        AZ_Assert(tkFramework, "TkFramework uninitialized when trying to create BlastFamily");

        // Create the TkActor from our Blast asset
        Nv::Blast::TkActorDesc tkActorDesc;
        {
            const NvBlastActorDesc& actorDesc = m_asset.GetPxAsset()->getDefaultActorDesc();

            // Initially all healths generated by houdini plugin must be 1, multiply them here to the value that is
            // specified in the material
            tkActorDesc.uniformInitialBondHealth = actorDesc.uniformInitialBondHealth * m_blastMaterial.GetHealth();
            tkActorDesc.uniformInitialLowerSupportChunkHealth =
                actorDesc.uniformInitialLowerSupportChunkHealth * desc.m_blastMaterial.GetHealth();
            // These must be nullptr, because we currently do not support non-uniform healths
            tkActorDesc.initialBondHealths = nullptr;
            tkActorDesc.initialSupportChunkHealths = nullptr;
            tkActorDesc.asset = &m_asset.GetPxAsset()->getTkAsset();
        }

        Nv::Blast::TkActor* actor = tkFramework->createActor(tkActorDesc);
        AZ_Assert(actor, "TkActor creation failed when creating BlastFamily.");

        // The new actor is the first member of a new TkFamily, which will be owned by BlastFamilyImpl.
        // Family takes care of releasing whatever actor is remaining.
        m_tkFamily.reset(&actor->getFamily());

        // If a TkGroup was passed in the description, add the new TkActor to it.
        // Actors will remove themselves from the group when they are released.
        // BlastSystemComponet takes care of destroying empty groups.
        if (desc.m_group)
        {
            desc.m_group->addActor(*actor);
        }
    }

    BlastFamilyImpl::~BlastFamilyImpl()
    {
        Despawn();
    }

    bool BlastFamilyImpl::Spawn(const AZ::Transform& transform)
    {
        AZ_Assert(m_tkFamily, "No TkFamily created for this BlastFamily.");
        if (m_isSpawned)
        {
            return false;
        }

        m_initialTransform = transform;

        m_tkFamily->addListener(*this);

        CreateActors(CalculateActorsDescFromFamily(transform));

        m_isSpawned = true;
        return true;
    }

    void BlastFamilyImpl::Despawn()
    {
        AZ_Assert(m_tkFamily, "No TkFamily created for this BlastFamily.");
        if (!m_isSpawned)
        {
            return;
        }

        // Intentional copy here as we will use this set to delete actors from ActorTracker itself
        AZStd::unordered_set<BlastActor*> toDelete = m_actorTracker.GetActors();
        DestroyActors(toDelete);

        m_tkFamily->removeListener(*this);
        m_isSpawned = false;
    }

    void BlastFamilyImpl::HandleEvents(const Nv::Blast::TkEvent* events, uint32_t eventCount)
    {
        AZ_PROFILE_FUNCTION(Physics);

        for (uint32_t i = 0; i < eventCount; ++i)
        {
            const Nv::Blast::TkEvent& event = events[i];
            switch (event.type)
            {
            case Nv::Blast::TkEvent::Split:
                {
                    AZStd::vector<BlastActorDesc> newActorsDesc;
                    AZStd::unordered_set<BlastActor*> actorsToDelete;

                    HandleSplitEvent(event.getPayload<Nv::Blast::TkSplitEvent>(), newActorsDesc, actorsToDelete);

                    DestroyActors(actorsToDelete);
                    CreateActors(AZStd::move(newActorsDesc));
                    break;
                }
            default:
                break;
            }
        }
    }

    void BlastFamilyImpl::HandleSplitEvent(
        const Nv::Blast::TkSplitEvent* splitEvent, AZStd::vector<BlastActorDesc>& newActorsDesc,
        AZStd::unordered_set<BlastActor*>& actorsToDelete)
    {
        AZ_PROFILE_FUNCTION(Physics);

        if (!splitEvent)
        {
            AZ_Error("Blast", false, "Received null TkSplitEvent from the Blast library.");
            return;
        }

        if (!splitEvent->parentData.userData)
        {
            AZ_Error("Blast", false, "Parent actor in split event must have user data.");
            return;
        }

        BlastActor* parentActor = static_cast<BlastActor*>(splitEvent->parentData.userData);
        AZ_Assert(parentActor, "TkActor had a null user data instead of a BlastActor.");

        AzPhysics::SimulatedBody* parentBody = parentActor->GetSimulatedBody();

        const uint32_t newActorsCount = splitEvent->numChildren;
        const bool parentStatic = parentActor->IsStatic();

        // Fill in actor create infos for newly created actors, based on the parent's velocity & center of mass
        for (uint32_t childIndex = 0; childIndex < newActorsCount; ++childIndex)
        {
            Nv::Blast::TkActor* tkActorChild = splitEvent->children[childIndex];
            if (!tkActorChild)
            {
                AZ_Error("Blast", false, "Split event generated with null TkActor");
                continue;
            }

            AZ::Transform parentTransform;
            if (parentBody)
            {
                parentTransform = parentBody->GetTransform();
                parentTransform.MultiplyByUniformScale(m_initialTransform.GetUniformScale());
            }
            else
            {
                parentTransform = m_initialTransform;
            }

            newActorsDesc.push_back(
                CalculateActorDesc(parentBody, parentStatic, parentTransform, tkActorChild));
        }

        actorsToDelete.insert(parentActor);
    }

    BlastActorDesc BlastFamilyImpl::CalculateActorDesc(
        AzPhysics::SimulatedBody* parentBody, bool parentStatic, AZ::Transform parentTransform, Nv::Blast::TkActor* tkActor)
    {
        auto actorDesc = CalculateActorDesc(parentTransform, tkActor);

        const bool isParentBodyDynamic = (parentBody && !parentStatic);

        actorDesc.m_bodyConfiguration.m_initialAngularVelocity =
            isParentBodyDynamic
            ? static_cast<AzPhysics::RigidBody*>(parentBody)->GetAngularVelocity()
            : AZ::Vector3::CreateZero();
        actorDesc.m_parentCenterOfMass = parentTransform.TransformPoint(
            isParentBodyDynamic
            ? static_cast<AzPhysics::RigidBody*>(parentBody)->GetCenterOfMassLocal()
            : AZ::Vector3::CreateZero());
        actorDesc.m_parentLinearVelocity =
            isParentBodyDynamic
            ? static_cast<AzPhysics::RigidBody*>(parentBody)->GetLinearVelocity()
            : AZ::Vector3::CreateZero();

        return actorDesc;
    }

    BlastActorDesc BlastFamilyImpl::CalculateActorDesc(const AZ::Transform& transform, Nv::Blast::TkActor* tkActor)
    {
        AzPhysics::RigidBodyConfiguration configuration;
        configuration.m_position = transform.GetTranslation();
        configuration.m_orientation = transform.GetRotation();
        configuration.m_ccdEnabled = m_actorConfiguration.m_isCcdEnabled;
        configuration.m_startSimulationEnabled = m_actorConfiguration.m_isSimulated;
        configuration.m_initialAngularVelocity = AZ::Vector3::CreateZero();

        BlastActorDesc actorDesc;
        actorDesc.m_family = this;
        actorDesc.m_tkActor = tkActor;
        actorDesc.m_physicsMaterialId = m_physicsMaterialId;
        actorDesc.m_chunkIndices = m_actorFactory->CalculateVisibleChunks(*this, *actorDesc.m_tkActor);
        actorDesc.m_isStatic = m_actorFactory->CalculateIsStatic(*this, *actorDesc.m_tkActor, actorDesc.m_chunkIndices);
        actorDesc.m_isLeafChunk = m_actorFactory->CalculateIsLeafChunk(*actorDesc.m_tkActor, actorDesc.m_chunkIndices);
        actorDesc.m_entity = m_entityProvider->CreateEntity(m_actorFactory->CalculateComponents(actorDesc.m_isStatic));
        actorDesc.m_parentCenterOfMass = transform.GetTranslation();
        actorDesc.m_parentLinearVelocity = AZ::Vector3::CreateZero();
        actorDesc.m_bodyConfiguration = configuration;
        actorDesc.m_scale = transform.GetUniformScale();

        return actorDesc;
    }

    void BlastFamilyImpl::CreateActors(const AZStd::vector<BlastActorDesc>& actorDescs)
    {
        AZ_PROFILE_FUNCTION(Physics);

        for (const auto& actorDesc : actorDescs)
        {
            BlastActor* actor = m_actorFactory->CreateActor(actorDesc);
            m_actorTracker.AddActor(actor);
            DispatchActorCreated(*actor);
        }
    }

    void BlastFamilyImpl::DestroyActors(const AZStd::unordered_set<BlastActor*>& actors)
    {
        AZ_PROFILE_FUNCTION(Physics);

        for (auto* actor : actors)
        {
            m_actorTracker.RemoveActor(actor);
            DispatchActorDestroyed(*actor);
            m_actorFactory->DestroyActor(actor);
        }
    }

    void BlastFamilyImpl::DestroyActor(BlastActor* blastActor)
    {
        if (!blastActor)
        {
            return;
        }

        if (m_actorTracker.GetActors().find(blastActor) == m_actorTracker.GetActors().end())
        {
            AZ_Warning(
                "Blast", false,
                "Family is trying to destroy actor that is not part of it. The actor is represented with entity id %s",
                blastActor->GetEntity()->GetId().ToString().c_str());
            return;
        }

        DestroyActors({blastActor});
    }

    void BlastFamilyImpl::DispatchActorCreated(const BlastActor& actor)
    {
        AZ_PROFILE_FUNCTION(Physics);

        m_listener->OnActorCreated(*this, actor);
    }

    void BlastFamilyImpl::DispatchActorDestroyed(const BlastActor& actor)
    {
        AZ_PROFILE_FUNCTION(Physics);

        m_listener->OnActorDestroyed(*this, actor);
    }

    AZStd::vector<BlastActorDesc> BlastFamilyImpl::CalculateActorsDescFromFamily(const AZ::Transform& transform)
    {
        // Get current active TkActors
        // Normally only 1, but it can be already in split state
        const uint32_t actorCount = m_tkFamily->getActorCount();
        AZStd::vector<Nv::Blast::TkActor*> initialTkActors(actorCount, nullptr);
        m_tkFamily->getActors(initialTkActors.data(), actorCount);

        // Fill initial actor create infos
        AZStd::vector<BlastActorDesc> initialActors;
        initialActors.reserve(actorCount);
        for (auto* tkActor : initialTkActors)
        {
            initialActors.push_back(CalculateActorDesc(transform, tkActor));
        }
        return initialActors;
    }

    static AZ::Color MixColors(const AZ::Color& color1, AZ::Color color2, float ratio)
    {
        return AZ::Color(
            color1.GetR() * (1 - ratio) + color2.GetR() * ratio, color1.GetG() * (1 - ratio) + color2.GetG() * ratio,
            color1.GetB() * (1 - ratio) + color2.GetB() * ratio, color1.GetA() * (1 - ratio) + color2.GetA() * ratio);
    }

    static AZ::Color bondHealthColor(float healthFraction)
    {
        const AZ::Color bondHealthyColor(0.0f, 1.0f, 0.0f, 1.0f);
        const AZ::Color bondMidColor(1.0f, 1.0f, 0.0f, 1.0f);
        const AZ::Color bondBrokenColor(1.0f, 0.0f, 0.0f, 1.0f);

        return healthFraction < 0.5 ? MixColors(bondBrokenColor, bondMidColor, 2.0f * healthFraction)
                                    : MixColors(bondMidColor, bondHealthyColor, 2.0f * healthFraction - 1.0f);
    }

    static void pushCentroid(
        AZStd::vector<DebugLine>& lines, AZ::Vector3 pos, AZ::Color color, const float& area, const AZ::Vector3& normal)
    {
        AZ_Assert(normal.IsNormalized(), "Provided normal must be normalized");
        // draw square of area 'area' rotated by normal
        {
            // build world rotation
            AZ::Vector3 n0(0, 0, 1);
            AZ::Vector3 n1 = normal;
            AZ::Vector3 axis = n0.Cross(n1);
            const float d = n0.Dot(n1);
            AZ::Quaternion q(axis, 1.f + d);
            q.Normalize();
            const float e = sqrt(1.0f / 2.0f);
            const float r = sqrt(area);

            // transform all 4 square points
            AZ::Transform t = AZ::Transform::CreateFromQuaternionAndTranslation(q, pos);
            AZ::Vector3 p0 = t.TransformPoint(AZ::Vector3(-e, e, 0) * r);
            AZ::Vector3 p1 = t.TransformPoint(AZ::Vector3(e, e, 0) * r);
            AZ::Vector3 p2 = t.TransformPoint(AZ::Vector3(e, -e, 0) * r);
            AZ::Vector3 p3 = t.TransformPoint(AZ::Vector3(-e, -e, 0) * r);

            if (p0.IsFinite()) {
                // push square edges
                lines.emplace_back(p0, p1, color);
                lines.emplace_back(p1, p2, color);
                lines.emplace_back(p2, p3, color);
                lines.emplace_back(p3, p0, color);
            }
        }

        // draw normal
        const AZ::Color bondNormalColor(0.0f, 0.8f, 1.0f, 1.0f);
        lines.emplace_back(pos, pos + normal * 0.5f, bondNormalColor);
    }

    void BlastFamilyImpl::FillDebugRenderHealthGraph(
        DebugRenderBuffer& debugRenderBuffer, DebugRenderMode mode, const Nv::Blast::TkActor& actor)
    {
        const NvBlastChunk* chunks = actor.getFamily().getAsset()->getChunks();
        const NvBlastBond* bonds = actor.getFamily().getAsset()->getBonds();
        const NvBlastSupportGraph graph = actor.getFamily().getAsset()->getGraph();
        const float bondHealthMax = m_asset.GetBondHealthMax() * m_blastMaterial.GetHealth();
        const uint32_t chunkCount = actor.getFamily().getAsset()->getChunkCount();

        uint32_t nodeCount = actor.getGraphNodeCount();
        std::vector<uint32_t> nodes(nodeCount);
        actor.getGraphNodeIndices(nodes.data(), aznumeric_cast<uint32_t>(nodes.size()));
        const float* bondHealths = actor.getBondHealths();
        const Nv::Blast::ExtPxChunk* pxChunks = m_asset.GetPxAsset()->getChunks();

        for (uint32_t node0 : nodes)
        {
            const uint32_t chunkIndex0 = graph.chunkIndices[node0];
            const NvBlastChunk& blastChunk0 = chunks[chunkIndex0];
            const Nv::Blast::ExtPxChunk& assetChunk0 = pxChunks[chunkIndex0];

            for (uint32_t adjacencyIndex = graph.adjacencyPartition[node0];
                 adjacencyIndex < graph.adjacencyPartition[node0 + 1]; adjacencyIndex++)
            {
                const uint32_t node1 = graph.adjacentNodeIndices[adjacencyIndex];
                const uint32_t chunkIndex1 = graph.chunkIndices[node1];
                const NvBlastChunk& blastChunk1 = chunks[chunkIndex1];
                const Nv::Blast::ExtPxChunk& assetChunk1 = pxChunks[chunkIndex1];
                if (node0 > node1)
                    continue;

                const bool invisibleBond = chunkIndex0 >= chunkCount || chunkIndex1 >= chunkCount ||
                    assetChunk0.subchunkCount == 0 || assetChunk1.subchunkCount == 0;

                // health
                const uint32_t bondIndex = graph.adjacentBondIndices[adjacencyIndex];
                const float healthVal = AZ::GetClamp(bondHealths[bondIndex] / bondHealthMax, 0.0f, 1.0f);

                AZ::Color color = bondHealthColor(healthVal);

                const NvBlastBond& solverBond = bonds[bondIndex];
                const AZ::Vector3 centroid(solverBond.centroid[0], solverBond.centroid[1], solverBond.centroid[2]);

                // centroid
                if (mode == DebugRenderHealthGraphCentroids || mode == DebugRenderCentroids)
                {
                    const AZ::Color bondInvisibleColor(0.65f, 0.16f, 0.16f, 1.0f);
                    const AZ::Vector3 normal(solverBond.normal[0], solverBond.normal[1], solverBond.normal[2]);
                    pushCentroid(
                        debugRenderBuffer.m_lines, centroid, (invisibleBond ? bondInvisibleColor : color),
                        solverBond.area, normal.GetNormalized());
                }

                // chunk connection (bond)
                if ((mode == DebugRenderHealthGraph || mode == DebugRenderHealthGraphCentroids) && !invisibleBond)
                {
                    const AZ::Vector3 c0(blastChunk0.centroid[0], blastChunk0.centroid[1], blastChunk0.centroid[2]);
                    const AZ::Vector3 c1(blastChunk1.centroid[0], blastChunk1.centroid[1], blastChunk1.centroid[2]);
                    debugRenderBuffer.m_lines.emplace_back(c0, c1, color);
                }
            }
        }
    }

    void BlastFamilyImpl::FillDebugRenderAccelerator(DebugRenderBuffer& debugRenderBuffer, DebugRenderMode mode)
    {
        if (m_asset.GetAccelerator())
        {
            const auto buffer = m_asset.GetAccelerator()->fillDebugRender(-1, mode == DebugRenderAabbTreeSegments);
            if (buffer.lineCount)
            {
                for (uint32_t i = 0; i < buffer.lineCount; ++i)
                {
                    auto& line = buffer.lines[i];
                    AZ::Color color;
                    color.FromU32(line.color0);
                    debugRenderBuffer.m_lines.emplace_back(
                        AZ::Vector3(line.pos0.x, line.pos0.y, line.pos0.z),
                        AZ::Vector3(line.pos1.x, line.pos1.y, line.pos1.z), color);
                }
            }
        }
    }

    void BlastFamilyImpl::FillDebugRender(DebugRenderBuffer& debugRenderBuffer, DebugRenderMode mode, [[maybe_unused]] float renderScale)
    {
        for (const BlastActor* blastActor : m_actorTracker.GetActors())
        {
            const Nv::Blast::TkActor& actor = blastActor->GetTkActor();
            auto lineStartIndex = aznumeric_cast<uint32_t>(debugRenderBuffer.m_lines.size());

            uint32_t nodeCount = actor.getGraphNodeCount();
            if (nodeCount == 0)
            {
                // subsupport chunks don't have graph nodes
                continue;
            }

            if (DebugRenderHealthGraph <= mode && mode <= DebugRenderHealthGraphCentroids)
            {
                FillDebugRenderHealthGraph(debugRenderBuffer, mode, actor);
            }

            if (mode == DebugRenderAabbTreeCentroids || mode == DebugRenderAabbTreeSegments)
            {
                FillDebugRenderAccelerator(debugRenderBuffer, mode);
            }

            // transform all added lines from local to global
            AZ::Transform localToGlobal = blastActor->GetSimulatedBody()->GetTransform();
            for (uint32_t i = lineStartIndex; i < debugRenderBuffer.m_lines.size(); i++)
            {
                DebugLine& line = debugRenderBuffer.m_lines[i];
                line.m_p0 = localToGlobal.TransformPoint(line.m_p0);
                line.m_p1 = localToGlobal.TransformPoint(line.m_p1);
            }
        }
    }

    ActorTracker& BlastFamilyImpl::GetActorTracker()
    {
        return m_actorTracker;
    }

    const Nv::Blast::TkFamily* BlastFamilyImpl::GetTkFamily() const
    {
        return m_tkFamily.get();
    }

    Nv::Blast::TkFamily* BlastFamilyImpl::GetTkFamily()
    {
        return m_tkFamily.get();
    }

    const Nv::Blast::ExtPxAsset& BlastFamilyImpl::GetPxAsset() const
    {
        AZ_Assert(m_asset.GetPxAsset(), "BlastFamily created with invalid ExtPxAsset.");
        return *m_asset.GetPxAsset();
    }

    void BlastFamilyImpl::receive(const Nv::Blast::TkEvent* events, uint32_t eventCount)
    {
        HandleEvents(events, eventCount);
    }

    const BlastActorConfiguration& BlastFamilyImpl::GetActorConfiguration() const
    {
        return m_actorConfiguration;
    }
} // namespace Blast
