//
//  EntityTreeRenderer.cpp
//  interface/src
//
//  Created by Brad Hefta-Gaub on 12/6/13.
//  Copyright 2013 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "EntityTreeRenderer.h"

#include <glm/gtx/quaternion.hpp>

#include <QEventLoop>
#include <QScriptSyntaxCheckResult>
#include <QThreadPool>

#include <shared/QtHelpers.h>
#include <ColorUtils.h>
#include <AbstractScriptingServicesInterface.h>
#include <AbstractViewStateInterface.h>
#include <Model.h>
#include <NetworkAccessManager.h>
#include <PerfStat.h>
#include <SceneScriptingInterface.h>
#include <ScriptEngine.h>


#include "RenderableEntityItem.h"

#include "RenderableLightEntityItem.h"
#include "RenderableModelEntityItem.h"
#include "RenderableParticleEffectEntityItem.h"
#include "RenderableTextEntityItem.h"
#include "RenderableWebEntityItem.h"
#include "RenderableZoneEntityItem.h"
#include "RenderableLineEntityItem.h"
#include "RenderablePolyVoxEntityItem.h"
#include "RenderablePolyLineEntityItem.h"
#include "RenderableShapeEntityItem.h"
#include "EntitiesRendererLogging.h"
#include "AddressManager.h"
#include <Rig.h>

#include "ZoneRenderer.h"

EntityTreeRenderer::EntityTreeRenderer(bool wantScripts, AbstractViewStateInterface* viewState,
                                            AbstractScriptingServicesInterface* scriptingServices) :
    _wantScripts(wantScripts),
    _lastPointerEventValid(false),
    _viewState(viewState),
    _scriptingServices(scriptingServices),
    _displayModelBounds(false),
    _dontDoPrecisionPicking(false),
    _layeredZones(this)
{
    REGISTER_ENTITY_TYPE_WITH_FACTORY(Model, RenderableModelEntityItem::factory)
    REGISTER_ENTITY_TYPE_WITH_FACTORY(Light, RenderableLightEntityItem::factory)
    REGISTER_ENTITY_TYPE_WITH_FACTORY(Text, RenderableTextEntityItem::factory)
    // Offscreen web surfaces are incompatible with nSight
    if (!nsightActive()) {
        REGISTER_ENTITY_TYPE_WITH_FACTORY(Web, RenderableWebEntityItem::factory)
    }
    REGISTER_ENTITY_TYPE_WITH_FACTORY(ParticleEffect, RenderableParticleEffectEntityItem::factory)
    REGISTER_ENTITY_TYPE_WITH_FACTORY(Zone, RenderableZoneEntityItem::factory)
    REGISTER_ENTITY_TYPE_WITH_FACTORY(Line, RenderableLineEntityItem::factory)
    REGISTER_ENTITY_TYPE_WITH_FACTORY(PolyVox, RenderablePolyVoxEntityItem::factory)
    REGISTER_ENTITY_TYPE_WITH_FACTORY(PolyLine, RenderablePolyLineEntityItem::factory)
    REGISTER_ENTITY_TYPE_WITH_FACTORY(Shape, RenderableShapeEntityItem::factory)
    REGISTER_ENTITY_TYPE_WITH_FACTORY(Box, RenderableShapeEntityItem::boxFactory)
    REGISTER_ENTITY_TYPE_WITH_FACTORY(Sphere, RenderableShapeEntityItem::sphereFactory)

    _currentHoverOverEntityID = UNKNOWN_ENTITY_ID;
    _currentClickingOnEntityID = UNKNOWN_ENTITY_ID;
}

EntityTreeRenderer::~EntityTreeRenderer() {
    // NOTE: We don't need to delete _entitiesScriptEngine because
    //       it is registered with ScriptEngines, which will call deleteLater for us.
}

int EntityTreeRenderer::_entitiesScriptEngineCount = 0;

void entitiesScriptEngineDeleter(ScriptEngine* engine) {
    class WaitRunnable : public QRunnable {
        public:
            WaitRunnable(ScriptEngine* engine) : _engine(engine) {}
            virtual void run() override {
                _engine->waitTillDoneRunning();
                _engine->deleteLater();
            }

        private:
            ScriptEngine* _engine;
    };

    // Wait for the scripting thread from the thread pool to avoid hanging the main thread
    QThreadPool::globalInstance()->start(new WaitRunnable(engine));
}

void EntityTreeRenderer::resetEntitiesScriptEngine() {
    // Keep a ref to oldEngine until newEngine is ready so EntityScriptingInterface has something to use
    auto oldEngine = _entitiesScriptEngine;

    auto newEngine = new ScriptEngine(ScriptEngine::ENTITY_CLIENT_SCRIPT, NO_SCRIPT, QString("about:Entities %1").arg(++_entitiesScriptEngineCount));
    _entitiesScriptEngine = QSharedPointer<ScriptEngine>(newEngine, entitiesScriptEngineDeleter);

    _scriptingServices->registerScriptEngineWithApplicationServices(_entitiesScriptEngine.data());
    _entitiesScriptEngine->runInThread();
    DependencyManager::get<EntityScriptingInterface>()->setEntitiesScriptEngine(_entitiesScriptEngine.data());
}

void EntityTreeRenderer::clear() {
    leaveAllEntities();

    // unload and stop the engine
    if (_entitiesScriptEngine) {
        // do this here (instead of in deleter) to avoid marshalling unload signals back to this thread
        _entitiesScriptEngine->unloadAllEntityScripts();
        _entitiesScriptEngine->stop();
    }

    // reset the engine
    if (_wantScripts && !_shuttingDown) {
        resetEntitiesScriptEngine();
    }

    // remove all entities from the scene
    auto scene = _viewState->getMain3DScene();
    if (scene) {
        render::Transaction transaction;
        foreach(auto entity, _entitiesInScene) {
            auto renderable = entity->getRenderableInterface();
            if (!renderable) {
                qCWarning(entitiesrenderer) << "EntityTreeRenderer::deletingEntity(), trying to remove non-renderable entity";
                continue;
            }
            renderable->removeFromScene(entity, scene, transaction);
        }
        scene->enqueueTransaction(transaction);
    } else {
        qCWarning(entitiesrenderer) << "EntitityTreeRenderer::clear(), Unexpected null scene, possibly during application shutdown";
    }
    _entitiesInScene.clear();

    // reset the zone to the default (while we load the next scene)
    _layeredZones.clear();

    OctreeProcessor::clear();
}

void EntityTreeRenderer::reloadEntityScripts() {
    _entitiesScriptEngine->unloadAllEntityScripts();
    _entitiesScriptEngine->resetModuleCache();
    foreach(auto entity, _entitiesInScene) {
        if (!entity->getScript().isEmpty()) {
            _entitiesScriptEngine->loadEntityScript(entity->getEntityItemID(), entity->getScript(), true);
        }
    }
}

void EntityTreeRenderer::init() {
    OctreeProcessor::init();
    EntityTreePointer entityTree = std::static_pointer_cast<EntityTree>(_tree);

    if (_wantScripts) {
        resetEntitiesScriptEngine();
    }

    forceRecheckEntities(); // setup our state to force checking our inside/outsideness of entities

    connect(entityTree.get(), &EntityTree::deletingEntity, this, &EntityTreeRenderer::deletingEntity, Qt::QueuedConnection);
    connect(entityTree.get(), &EntityTree::addingEntity, this, &EntityTreeRenderer::addingEntity, Qt::QueuedConnection);
    connect(entityTree.get(), &EntityTree::entityScriptChanging,
            this, &EntityTreeRenderer::entityScriptChanging, Qt::QueuedConnection);
}

void EntityTreeRenderer::shutdown() {
    if (_entitiesScriptEngine) {
        _entitiesScriptEngine->disconnectNonEssentialSignals(); // disconnect all slots/signals from the script engine, except essential
    }
    _shuttingDown = true;

    clear(); // always clear() on shutdown
}

void EntityTreeRenderer::setTree(OctreePointer newTree) {
    OctreeProcessor::setTree(newTree);
}

void EntityTreeRenderer::update() {
    PerformanceTimer perfTimer("ETRupdate");
    if (_tree && !_shuttingDown) {
        EntityTreePointer tree = std::static_pointer_cast<EntityTree>(_tree);
        tree->update();

        // Handle enter/leave entity logic
        checkEnterLeaveEntities();

        // Even if we're not moving the mouse, if we started clicking on an entity and we have
        // not yet released the hold then this is still considered a holdingClickOnEntity event
        // and we want to simulate this message here as well as in mouse move
        if (_lastPointerEventValid && !_currentClickingOnEntityID.isInvalidID()) {
            emit holdingClickOnEntity(_currentClickingOnEntityID, _lastPointerEvent);
            _entitiesScriptEngine->callEntityScriptMethod(_currentClickingOnEntityID, "holdingClickOnEntity", _lastPointerEvent);
        }

    }
    deleteReleasedModels();
}

bool EntityTreeRenderer::findBestZoneAndMaybeContainingEntities(QVector<EntityItemID>* entitiesContainingAvatar) {
    bool didUpdate = false;
    float radius = 0.01f; // for now, assume 0.01 meter radius, because we actually check the point inside later
    QVector<EntityItemPointer> foundEntities;

    // find the entities near us
    // don't let someone else change our tree while we search
    _tree->withReadLock([&] {

        // FIXME - if EntityTree had a findEntitiesContainingPoint() this could theoretically be a little faster
        std::static_pointer_cast<EntityTree>(_tree)->findEntities(_avatarPosition, radius, foundEntities);

        LayeredZones oldLayeredZones(std::move(_layeredZones));
        _layeredZones.clear();

        // create a list of entities that actually contain the avatar's position
        for (auto& entity : foundEntities) {
            auto isZone = entity->getType() == EntityTypes::Zone;
            auto hasScript = !entity->getScript().isEmpty();

            // only consider entities that are zones or have scripts, all other entities can
            // be ignored because they can have events fired on them.
            // FIXME - this could be optimized further by determining if the script is loaded
            // and if it has either an enterEntity or leaveEntity method
            if (isZone || hasScript) {
                // now check to see if the point contains our entity, this can be expensive if
                // the entity has a collision hull
                if (entity->contains(_avatarPosition)) {
                    if (entitiesContainingAvatar) {
                        *entitiesContainingAvatar << entity->getEntityItemID();
                    }

                    // if this entity is a zone and visible, determine if it is the bestZone
                    if (isZone && entity->getVisible()) {
                        auto renderID = std::dynamic_pointer_cast<RenderableZoneEntityItem>(entity)->getRenderItemID();
                        bool isValidRenderID = (renderID != render::Item::INVALID_ITEM_ID);

                        if (isValidRenderID) {
                            auto zone = std::dynamic_pointer_cast<ZoneEntityItem>(entity);
                            _layeredZones.insert(zone);
                        }
                    }
                }
            }
        }

        // check if our layered zones have changed
        if (_layeredZones.empty()) {
            if (oldLayeredZones.empty()) {
                return;
            }
        } else if (!oldLayeredZones.empty()) {
            if (_layeredZones.contains(oldLayeredZones)) {
                return;
            }
        }
        _layeredZones.apply();

        applyLayeredZones();

        didUpdate = true;
    });

    return didUpdate;
}

bool EntityTreeRenderer::checkEnterLeaveEntities() {
    PerformanceTimer perfTimer("checkEnterLeaveEntities");
    auto now = usecTimestampNow();
    bool didUpdate = false;

    if (_tree && !_shuttingDown) {
        glm::vec3 avatarPosition = _viewState->getAvatarPosition();

        // we want to check our enter/leave state if we've moved a significant amount, or
        // if some amount of time has elapsed since we last checked. We check the time
        // elapsed because zones or entities might have been created "around us" while we've
        // been stationary
        auto movedEnough = glm::distance(avatarPosition, _avatarPosition) > ZONE_CHECK_DISTANCE;
        auto enoughTimeElapsed = (now - _lastZoneCheck) > ZONE_CHECK_INTERVAL;
        
        if (movedEnough || enoughTimeElapsed) {
            _avatarPosition = avatarPosition;
            _lastZoneCheck = now;
            QVector<EntityItemID> entitiesContainingAvatar;
            didUpdate = findBestZoneAndMaybeContainingEntities(&entitiesContainingAvatar);
            
            // Note: at this point we don't need to worry about the tree being locked, because we only deal with
            // EntityItemIDs from here. The callEntityScriptMethod() method is robust against attempting to call scripts
            // for entity IDs that no longer exist.

            // for all of our previous containing entities, if they are no longer containing then send them a leave event
            foreach(const EntityItemID& entityID, _currentEntitiesInside) {
                if (!entitiesContainingAvatar.contains(entityID)) {
                    emit leaveEntity(entityID);
                    if (_entitiesScriptEngine) {
                        _entitiesScriptEngine->callEntityScriptMethod(entityID, "leaveEntity");
                    }
                }
            }

            // for all of our new containing entities, if they weren't previously containing then send them an enter event
            foreach(const EntityItemID& entityID, entitiesContainingAvatar) {
                if (!_currentEntitiesInside.contains(entityID)) {
                    emit enterEntity(entityID);
                    if (_entitiesScriptEngine) {
                        _entitiesScriptEngine->callEntityScriptMethod(entityID, "enterEntity");
                    }
                }
            }
            _currentEntitiesInside = entitiesContainingAvatar;
        }
    }
    return didUpdate;
}

void EntityTreeRenderer::leaveAllEntities() {
    if (_tree && !_shuttingDown) {

        // for all of our previous containing entities, if they are no longer containing then send them a leave event
        foreach(const EntityItemID& entityID, _currentEntitiesInside) {
            emit leaveEntity(entityID);
            if (_entitiesScriptEngine) {
                _entitiesScriptEngine->callEntityScriptMethod(entityID, "leaveEntity");
            }
        }
        _currentEntitiesInside.clear();
        forceRecheckEntities();
    }
}

void EntityTreeRenderer::forceRecheckEntities() {
    // make sure our "last avatar position" is something other than our current position, 
    // so that on our next chance, we'll check for enter/leave entity events.
    _avatarPosition = _viewState->getAvatarPosition() + glm::vec3((float)TREE_SCALE);
}

bool EntityTreeRenderer::applyLayeredZones() {
    // from the list of zones we are going to build a selection list the Render Item corresponding to the zones
    // in the expected layered order and update the scene with it
    auto scene = _viewState->getMain3DScene();
    if (scene) {
        render::Transaction transaction;
        render::ItemIDs list;

        for (auto& zone : _layeredZones) {
            auto id = std::dynamic_pointer_cast<RenderableZoneEntityItem>(zone.zone)->getRenderItemID();
            Q_ASSERT(id != render::Item::INVALID_ITEM_ID);
            list.push_back(id);
        }
        render::Selection selection("RankedZones", list);
        transaction.resetSelection(selection);

        scene->enqueueTransaction(transaction);
    } else {
        qCWarning(entitiesrenderer) << "EntityTreeRenderer::applyLayeredZones(), Unexpected null scene, possibly during application shutdown";
    }
     
     return true;
}

void EntityTreeRenderer::processEraseMessage(ReceivedMessage& message, const SharedNodePointer& sourceNode) {
    std::static_pointer_cast<EntityTree>(_tree)->processEraseMessage(message, sourceNode);
}

ModelPointer EntityTreeRenderer::allocateModel(const QString& url, float loadingPriority, SpatiallyNestable* spatiallyNestableOverride) {
    ModelPointer model = nullptr;

    // Only create and delete models on the thread that owns the EntityTreeRenderer
    if (QThread::currentThread() != thread()) {
        BLOCKING_INVOKE_METHOD(this, "allocateModel",
                Q_RETURN_ARG(ModelPointer, model),
                Q_ARG(const QString&, url));

        return model;
    }

    model = std::make_shared<Model>(nullptr, spatiallyNestableOverride);
    model->setLoadingPriority(loadingPriority);
    model->init();
    model->setURL(QUrl(url));
    return model;
}

ModelPointer EntityTreeRenderer::updateModel(ModelPointer model, const QString& newUrl) {
    // Only create and delete models on the thread that owns the EntityTreeRenderer
    if (QThread::currentThread() != thread()) {
        BLOCKING_INVOKE_METHOD(this, "updateModel",
            Q_RETURN_ARG(ModelPointer, model),
                Q_ARG(ModelPointer, model),
                Q_ARG(const QString&, newUrl));

        return model;
    }

    model->setURL(QUrl(newUrl));
    return model;
}

void EntityTreeRenderer::releaseModel(ModelPointer model) {
    // If we're not on the renderer's thread, then remember this model to be deleted later
    if (QThread::currentThread() != thread()) {
        _releasedModels << model;
    } else { // otherwise just delete it right away
        model.reset();
    }
}

void EntityTreeRenderer::deleteReleasedModels() {
    if (_releasedModels.size() > 0) {
        foreach(ModelPointer model, _releasedModels) {
            model.reset();
        }
        _releasedModels.clear();
    }
}

RayToEntityIntersectionResult EntityTreeRenderer::findRayIntersectionWorker(const PickRay& ray, Octree::lockType lockType,
                                                                                    bool precisionPicking, const QVector<EntityItemID>& entityIdsToInclude,
                                                                                    const QVector<EntityItemID>& entityIdsToDiscard, bool visibleOnly, bool collidableOnly) {
    RayToEntityIntersectionResult result;
    if (_tree) {
        EntityTreePointer entityTree = std::static_pointer_cast<EntityTree>(_tree);

        OctreeElementPointer element;
        EntityItemPointer intersectedEntity = NULL;
        result.intersects = entityTree->findRayIntersection(ray.origin, ray.direction,
            entityIdsToInclude, entityIdsToDiscard, visibleOnly, collidableOnly, precisionPicking,
            element, result.distance, result.face, result.surfaceNormal,
            (void**)&intersectedEntity, lockType, &result.accurate);
        if (result.intersects && intersectedEntity) {
            result.entityID = intersectedEntity->getEntityItemID();
            result.intersection = ray.origin + (ray.direction * result.distance);
            result.entity = intersectedEntity;
        }
    }
    return result;
}

void EntityTreeRenderer::connectSignalsToSlots(EntityScriptingInterface* entityScriptingInterface) {

    connect(this, &EntityTreeRenderer::mousePressOnEntity, entityScriptingInterface, &EntityScriptingInterface::mousePressOnEntity);
    connect(this, &EntityTreeRenderer::mouseMoveOnEntity, entityScriptingInterface, &EntityScriptingInterface::mouseMoveOnEntity);
    connect(this, &EntityTreeRenderer::mouseReleaseOnEntity, entityScriptingInterface, &EntityScriptingInterface::mouseReleaseOnEntity);

    connect(this, &EntityTreeRenderer::clickDownOnEntity, entityScriptingInterface, &EntityScriptingInterface::clickDownOnEntity);
    connect(this, &EntityTreeRenderer::holdingClickOnEntity, entityScriptingInterface, &EntityScriptingInterface::holdingClickOnEntity);
    connect(this, &EntityTreeRenderer::clickReleaseOnEntity, entityScriptingInterface, &EntityScriptingInterface::clickReleaseOnEntity);

    connect(this, &EntityTreeRenderer::hoverEnterEntity, entityScriptingInterface, &EntityScriptingInterface::hoverEnterEntity);
    connect(this, &EntityTreeRenderer::hoverOverEntity, entityScriptingInterface, &EntityScriptingInterface::hoverOverEntity);
    connect(this, &EntityTreeRenderer::hoverLeaveEntity, entityScriptingInterface, &EntityScriptingInterface::hoverLeaveEntity);

    connect(this, &EntityTreeRenderer::enterEntity, entityScriptingInterface, &EntityScriptingInterface::enterEntity);
    connect(this, &EntityTreeRenderer::leaveEntity, entityScriptingInterface, &EntityScriptingInterface::leaveEntity);
    connect(this, &EntityTreeRenderer::collisionWithEntity, entityScriptingInterface, &EntityScriptingInterface::collisionWithEntity);

    connect(DependencyManager::get<SceneScriptingInterface>().data(), &SceneScriptingInterface::shouldRenderEntitiesChanged, this, &EntityTreeRenderer::updateEntityRenderStatus, Qt::QueuedConnection);
}

static glm::vec2 projectOntoEntityXYPlane(EntityItemPointer entity, const PickRay& pickRay, const RayToEntityIntersectionResult& rayPickResult) {

    if (entity) {

        glm::vec3 entityPosition = entity->getPosition();
        glm::quat entityRotation = entity->getRotation();
        glm::vec3 entityDimensions = entity->getDimensions();
        glm::vec3 entityRegistrationPoint = entity->getRegistrationPoint();

        // project the intersection point onto the local xy plane of the object.
        float distance;
        glm::vec3 planePosition = entityPosition;
        glm::vec3 planeNormal = entityRotation * Vectors::UNIT_Z;
        glm::vec3 rayDirection = pickRay.direction;
        glm::vec3 rayStart = pickRay.origin;
        glm::vec3 p;
        if (rayPlaneIntersection(planePosition, planeNormal, rayStart, rayDirection, distance)) {
            p = rayStart + rayDirection * distance;
        } else {
            p = rayPickResult.intersection;
        }
        glm::vec3 localP = glm::inverse(entityRotation) * (p - entityPosition);
        glm::vec3 normalizedP = (localP / entityDimensions) + entityRegistrationPoint;
        return glm::vec2(normalizedP.x * entityDimensions.x,
                         (1.0f - normalizedP.y) * entityDimensions.y);  // flip y-axis
    } else {
        return glm::vec2();
    }
}

static uint32_t toPointerButtons(const QMouseEvent& event) {
    uint32_t buttons = 0;
    buttons |= event.buttons().testFlag(Qt::LeftButton) ? PointerEvent::PrimaryButton : 0;
    buttons |= event.buttons().testFlag(Qt::RightButton) ? PointerEvent::SecondaryButton : 0;
    buttons |= event.buttons().testFlag(Qt::MiddleButton) ? PointerEvent::TertiaryButton : 0;
    return buttons;
}

static PointerEvent::Button toPointerButton(const QMouseEvent& event) {
    switch (event.button()) {
    case Qt::LeftButton:
        return PointerEvent::PrimaryButton;
    case Qt::RightButton:
        return PointerEvent::SecondaryButton;
    case Qt::MiddleButton:
        return PointerEvent::TertiaryButton;
    default:
        return PointerEvent::NoButtons;
    }
}

static const uint32_t MOUSE_POINTER_ID = 0;

void EntityTreeRenderer::mousePressEvent(QMouseEvent* event) {
    // If we don't have a tree, or we're in the process of shutting down, then don't
    // process these events.
    if (!_tree || _shuttingDown) {
        return;
    }
    PerformanceTimer perfTimer("EntityTreeRenderer::mousePressEvent");
    PickRay ray = _viewState->computePickRay(event->x(), event->y());

    bool precisionPicking = !_dontDoPrecisionPicking;
    RayToEntityIntersectionResult rayPickResult = findRayIntersectionWorker(ray, Octree::Lock, precisionPicking);
    if (rayPickResult.intersects) {
        //qCDebug(entitiesrenderer) << "mousePressEvent over entity:" << rayPickResult.entityID;

        auto entity = getTree()->findEntityByEntityItemID(rayPickResult.entityID);
        auto properties = entity->getProperties();
        QString urlString = properties.getHref();
        QUrl url = QUrl(urlString, QUrl::StrictMode);
        if (url.isValid() && !url.isEmpty()){
            DependencyManager::get<AddressManager>()->handleLookupString(urlString);
        }

        glm::vec2 pos2D = projectOntoEntityXYPlane(rayPickResult.entity, ray, rayPickResult);
        PointerEvent pointerEvent(PointerEvent::Press, MOUSE_POINTER_ID,
                                  pos2D, rayPickResult.intersection,
                                  rayPickResult.surfaceNormal, ray.direction,
                                  toPointerButton(*event), toPointerButtons(*event),
                                  Qt::NoModifier); // TODO -- check for modifier keys?

        emit mousePressOnEntity(rayPickResult.entityID, pointerEvent);

        if (_entitiesScriptEngine) {
            _entitiesScriptEngine->callEntityScriptMethod(rayPickResult.entityID, "mousePressOnEntity", pointerEvent);
        }

        _currentClickingOnEntityID = rayPickResult.entityID;
        emit clickDownOnEntity(_currentClickingOnEntityID, pointerEvent);
        if (_entitiesScriptEngine) {
            _entitiesScriptEngine->callEntityScriptMethod(_currentClickingOnEntityID, "clickDownOnEntity", pointerEvent);
        }

        _lastPointerEvent = pointerEvent;
        _lastPointerEventValid = true;

    } else {
        emit mousePressOffEntity();
    }
}

void EntityTreeRenderer::mouseDoublePressEvent(QMouseEvent* event) {
    // If we don't have a tree, or we're in the process of shutting down, then don't
    // process these events.
    if (!_tree || _shuttingDown) {
        return;
    }
    PerformanceTimer perfTimer("EntityTreeRenderer::mouseDoublePressEvent");
    PickRay ray = _viewState->computePickRay(event->x(), event->y());

    bool precisionPicking = !_dontDoPrecisionPicking;
    RayToEntityIntersectionResult rayPickResult = findRayIntersectionWorker(ray, Octree::Lock, precisionPicking);
    if (rayPickResult.intersects) {
        //qCDebug(entitiesrenderer) << "mouseDoublePressEvent over entity:" << rayPickResult.entityID;

        glm::vec2 pos2D = projectOntoEntityXYPlane(rayPickResult.entity, ray, rayPickResult);
        PointerEvent pointerEvent(PointerEvent::Press, MOUSE_POINTER_ID,
            pos2D, rayPickResult.intersection,
            rayPickResult.surfaceNormal, ray.direction,
            toPointerButton(*event), toPointerButtons(*event), Qt::NoModifier);

        emit mouseDoublePressOnEntity(rayPickResult.entityID, pointerEvent);

        if (_entitiesScriptEngine) {
            _entitiesScriptEngine->callEntityScriptMethod(rayPickResult.entityID, "mouseDoublePressOnEntity", pointerEvent);
        }

        _currentClickingOnEntityID = rayPickResult.entityID;
        emit clickDownOnEntity(_currentClickingOnEntityID, pointerEvent);
        if (_entitiesScriptEngine) {
            _entitiesScriptEngine->callEntityScriptMethod(_currentClickingOnEntityID, "doubleclickOnEntity", pointerEvent);
        }

        _lastPointerEvent = pointerEvent;
        _lastPointerEventValid = true;

    } else {
        emit mouseDoublePressOffEntity();
    }
}

void EntityTreeRenderer::mouseReleaseEvent(QMouseEvent* event) {
    // If we don't have a tree, or we're in the process of shutting down, then don't
    // process these events.
    if (!_tree || _shuttingDown) {
        return;
    }

    PerformanceTimer perfTimer("EntityTreeRenderer::mouseReleaseEvent");
    PickRay ray = _viewState->computePickRay(event->x(), event->y());
    bool precisionPicking = !_dontDoPrecisionPicking;
    RayToEntityIntersectionResult rayPickResult = findRayIntersectionWorker(ray, Octree::Lock, precisionPicking);
    if (rayPickResult.intersects) {
        //qCDebug(entitiesrenderer) << "mouseReleaseEvent over entity:" << rayPickResult.entityID;

        glm::vec2 pos2D = projectOntoEntityXYPlane(rayPickResult.entity, ray, rayPickResult);
        PointerEvent pointerEvent(PointerEvent::Release, MOUSE_POINTER_ID,
                                  pos2D, rayPickResult.intersection,
                                  rayPickResult.surfaceNormal, ray.direction,
                                  toPointerButton(*event), toPointerButtons(*event),
                                  Qt::NoModifier); // TODO -- check for modifier keys?

        emit mouseReleaseOnEntity(rayPickResult.entityID, pointerEvent);
        if (_entitiesScriptEngine) {
            _entitiesScriptEngine->callEntityScriptMethod(rayPickResult.entityID, "mouseReleaseOnEntity", pointerEvent);
        }

        _lastPointerEvent = pointerEvent;
        _lastPointerEventValid = true;
    }

    // Even if we're no longer intersecting with an entity, if we started clicking on it, and now
    // we're releasing the button, then this is considered a clickOn event
    if (!_currentClickingOnEntityID.isInvalidID()) {

        auto entity = getTree()->findEntityByID(_currentClickingOnEntityID);
        glm::vec2 pos2D = projectOntoEntityXYPlane(entity, ray, rayPickResult);
        PointerEvent pointerEvent(PointerEvent::Release, MOUSE_POINTER_ID,
                                  pos2D, rayPickResult.intersection,
                                  rayPickResult.surfaceNormal, ray.direction,
                                  toPointerButton(*event), toPointerButtons(*event),
                                  Qt::NoModifier); // TODO -- check for modifier keys?

        emit clickReleaseOnEntity(_currentClickingOnEntityID, pointerEvent);
        if (_entitiesScriptEngine) {
            _entitiesScriptEngine->callEntityScriptMethod(rayPickResult.entityID, "clickReleaseOnEntity", pointerEvent);
        }
    }

    // makes it the unknown ID, we just released so we can't be clicking on anything
    _currentClickingOnEntityID = UNKNOWN_ENTITY_ID;
}

void EntityTreeRenderer::mouseMoveEvent(QMouseEvent* event) {
    // If we don't have a tree, or we're in the process of shutting down, then don't
    // process these events.
    if (!_tree || _shuttingDown) {
        return;
    }
    PerformanceTimer perfTimer("EntityTreeRenderer::mouseMoveEvent");

    PickRay ray = _viewState->computePickRay(event->x(), event->y());

    bool precisionPicking = false; // for mouse moves we do not do precision picking
    RayToEntityIntersectionResult rayPickResult = findRayIntersectionWorker(ray, Octree::TryLock, precisionPicking);
    if (rayPickResult.intersects) {

        glm::vec2 pos2D = projectOntoEntityXYPlane(rayPickResult.entity, ray, rayPickResult);
        PointerEvent pointerEvent(PointerEvent::Move, MOUSE_POINTER_ID,
                                  pos2D, rayPickResult.intersection,
                                  rayPickResult.surfaceNormal, ray.direction,
                                  toPointerButton(*event), toPointerButtons(*event),
                                  Qt::NoModifier); // TODO -- check for modifier keys?

        emit mouseMoveOnEntity(rayPickResult.entityID, pointerEvent);

        if (_entitiesScriptEngine) {
            _entitiesScriptEngine->callEntityScriptMethod(rayPickResult.entityID, "mouseMoveEvent", pointerEvent);
            _entitiesScriptEngine->callEntityScriptMethod(rayPickResult.entityID, "mouseMoveOnEntity", pointerEvent);
        }

        // handle the hover logic...

        // if we were previously hovering over an entity, and this new entity is not the same as our previous entity
        // then we need to send the hover leave.
        if (!_currentHoverOverEntityID.isInvalidID() && rayPickResult.entityID != _currentHoverOverEntityID) {

            auto entity = getTree()->findEntityByID(_currentHoverOverEntityID);
            glm::vec2 pos2D = projectOntoEntityXYPlane(entity, ray, rayPickResult);
            PointerEvent pointerEvent(PointerEvent::Move, MOUSE_POINTER_ID,
                                      pos2D, rayPickResult.intersection,
                                      rayPickResult.surfaceNormal, ray.direction,
                                      toPointerButton(*event), toPointerButtons(*event),
                                      Qt::NoModifier); // TODO -- check for modifier keys?

            emit hoverLeaveEntity(_currentHoverOverEntityID, pointerEvent);
            if (_entitiesScriptEngine) {
                _entitiesScriptEngine->callEntityScriptMethod(_currentHoverOverEntityID, "hoverLeaveEntity", pointerEvent);
            }
        }

        // If the new hover entity does not match the previous hover entity then we are entering the new one
        // this is true if the _currentHoverOverEntityID is known or unknown
        if (rayPickResult.entityID != _currentHoverOverEntityID) {
            emit hoverEnterEntity(rayPickResult.entityID, pointerEvent);
            if (_entitiesScriptEngine) {
                _entitiesScriptEngine->callEntityScriptMethod(rayPickResult.entityID, "hoverEnterEntity", pointerEvent);
            }
        }

        // and finally, no matter what, if we're intersecting an entity then we're definitely hovering over it, and
        // we should send our hover over event
        emit hoverOverEntity(rayPickResult.entityID, pointerEvent);
        if (_entitiesScriptEngine) {
            _entitiesScriptEngine->callEntityScriptMethod(rayPickResult.entityID, "hoverOverEntity", pointerEvent);
        }

        // remember what we're hovering over
        _currentHoverOverEntityID = rayPickResult.entityID;

        _lastPointerEvent = pointerEvent;
        _lastPointerEventValid = true;

    } else {
        // handle the hover logic...
        // if we were previously hovering over an entity, and we're no longer hovering over any entity then we need to
        // send the hover leave for our previous entity
        if (!_currentHoverOverEntityID.isInvalidID()) {

            auto entity = getTree()->findEntityByID(_currentHoverOverEntityID);
            glm::vec2 pos2D = projectOntoEntityXYPlane(entity, ray, rayPickResult);
            PointerEvent pointerEvent(PointerEvent::Move, MOUSE_POINTER_ID,
                                  pos2D, rayPickResult.intersection,
                                  rayPickResult.surfaceNormal, ray.direction,
                                      toPointerButton(*event), toPointerButtons(*event),
                                      Qt::NoModifier); // TODO -- check for modifier keys?

            emit hoverLeaveEntity(_currentHoverOverEntityID, pointerEvent);
            if (_entitiesScriptEngine) {
                _entitiesScriptEngine->callEntityScriptMethod(_currentHoverOverEntityID, "hoverLeaveEntity", pointerEvent);
            }
            _currentHoverOverEntityID = UNKNOWN_ENTITY_ID; // makes it the unknown ID
        }
    }

    // Even if we're no longer intersecting with an entity, if we started clicking on an entity and we have
    // not yet released the hold then this is still considered a holdingClickOnEntity event
    if (!_currentClickingOnEntityID.isInvalidID()) {

        auto entity = getTree()->findEntityByID(_currentClickingOnEntityID);
        glm::vec2 pos2D = projectOntoEntityXYPlane(entity, ray, rayPickResult);
        PointerEvent pointerEvent(PointerEvent::Move, MOUSE_POINTER_ID,
                                  pos2D, rayPickResult.intersection,
                                  rayPickResult.surfaceNormal, ray.direction,
                                  toPointerButton(*event), toPointerButtons(*event),
                                  Qt::NoModifier); // TODO -- check for modifier keys?

        emit holdingClickOnEntity(_currentClickingOnEntityID, pointerEvent);
        if (_entitiesScriptEngine) {
            _entitiesScriptEngine->callEntityScriptMethod(_currentClickingOnEntityID, "holdingClickOnEntity", pointerEvent);
        }
    }
}

void EntityTreeRenderer::deletingEntity(const EntityItemID& entityID) {
    if (!_entitiesInScene.contains(entityID)) {
        return;
    }
        
    if (_tree && !_shuttingDown && _entitiesScriptEngine) {
        _entitiesScriptEngine->unloadEntityScript(entityID, true);
    }

    auto scene = _viewState->getMain3DScene();
    if (!scene) {
        qCWarning(entitiesrenderer) << "EntityTreeRenderer::deletingEntity(), Unexpected null scene, possibly during application shutdown";
        return;
    }

    auto entity = _entitiesInScene.take(entityID);
    auto renderable = entity->getRenderableInterface();
    if (!renderable) {
        qCWarning(entitiesrenderer) << "EntityTreeRenderer::deletingEntity(), trying to remove non-renderable entity";
        return;
    }

    forceRecheckEntities(); // reset our state to force checking our inside/outsideness of entities

    // here's where we remove the entity payload from the scene
    render::Transaction transaction;
    renderable->removeFromScene(entity, scene, transaction);
    scene->enqueueTransaction(transaction);
}

void EntityTreeRenderer::addingEntity(const EntityItemID& entityID) {
    forceRecheckEntities(); // reset our state to force checking our inside/outsideness of entities
    checkAndCallPreload(entityID);
    auto entity = std::static_pointer_cast<EntityTree>(_tree)->findEntityByID(entityID);
    if (entity) {
        addEntityToScene(entity);
    }
}

void EntityTreeRenderer::addEntityToScene(const EntityItemPointer& entity) {
    // here's where we add the entity payload to the scene
    auto scene = _viewState->getMain3DScene();
    if (!scene) {
        qCWarning(entitiesrenderer) << "EntityTreeRenderer::addEntityToScene(), Unexpected null scene, possibly during application shutdown";
        return;
    }

    auto renderable = entity->getRenderableInterface();
    if (!renderable) {
        qCWarning(entitiesrenderer) << "EntityTreeRenderer::addEntityToScene(), Unexpected non-renderable entity";
        return;
    }

    render::Transaction transaction;
    if (renderable->addToScene(entity, scene, transaction)) {
        _entitiesInScene.insert(entity->getEntityItemID(), entity);
    }
    scene->enqueueTransaction(transaction);
}


void EntityTreeRenderer::entityScriptChanging(const EntityItemID& entityID, bool reload) {
    checkAndCallPreload(entityID, reload, true);
}

void EntityTreeRenderer::checkAndCallPreload(const EntityItemID& entityID, bool reload, bool unloadFirst) {
    if (_tree && !_shuttingDown) {
        EntityItemPointer entity = getTree()->findEntityByEntityItemID(entityID);
        if (!entity) {
            return;
        }
        bool shouldLoad = entity->shouldPreloadScript() && _entitiesScriptEngine;
        QString scriptUrl = entity->getScript();
        if ((shouldLoad && unloadFirst) || scriptUrl.isEmpty()) {
            _entitiesScriptEngine->unloadEntityScript(entityID);
            entity->scriptHasUnloaded();
        }
        if (shouldLoad) {
            scriptUrl = DependencyManager::get<ResourceManager>()->normalizeURL(scriptUrl);
            _entitiesScriptEngine->loadEntityScript(entityID, scriptUrl, reload);
            entity->scriptHasPreloaded();
        }
    }
}

void EntityTreeRenderer::playEntityCollisionSound(EntityItemPointer entity, const Collision& collision) {
    assert((bool)entity);
    auto renderable = entity->getRenderableInterface();
    if (!renderable) { 
        return; 
    }
    
    SharedSoundPointer collisionSound = renderable->getCollisionSound();
    if (!collisionSound) {
        return;
    }
    bool success = false;
    AACube minAACube = entity->getMinimumAACube(success);
    if (!success) {
        return;
    }
    float mass = entity->computeMass();

    const float COLLISION_PENETRATION_TO_VELOCITY = 50.0f; // as a subsitute for RELATIVE entity->getVelocity()
    // The collision.penetration is a pretty good indicator of changed velocity AFTER the initial contact,
    // but that first contact depends on exactly where we hit in the physics step.
    // We can get a more consistent initial-contact energy reading by using the changed velocity.
    // Note that velocityChange is not a good indicator for continuing collisions, because it does not distinguish
    // between bounce and sliding along a surface.
    const float speedSquared = (collision.type == CONTACT_EVENT_TYPE_START) ?
        glm::length2(collision.velocityChange) :
        glm::length2(collision.penetration) * COLLISION_PENETRATION_TO_VELOCITY;
    const float energy = mass * speedSquared / 2.0f;
    const float COLLISION_ENERGY_AT_FULL_VOLUME = (collision.type == CONTACT_EVENT_TYPE_START) ? 150.0f : 5.0f;
    const float COLLISION_MINIMUM_VOLUME = 0.005f;
    const float energyFactorOfFull = fmin(1.0f, energy / COLLISION_ENERGY_AT_FULL_VOLUME);
    if (energyFactorOfFull < COLLISION_MINIMUM_VOLUME) {
        return;
    }
    // Quiet sound aren't really heard at all, so we can compress everything to the range [1-c, 1], if we play it all.
    const float COLLISION_SOUND_COMPRESSION_RANGE = 1.0f; // This section could be removed when the value is 1, but let's see how it goes.
    const float volume = (energyFactorOfFull * COLLISION_SOUND_COMPRESSION_RANGE) + (1.0f - COLLISION_SOUND_COMPRESSION_RANGE);

    // Shift the pitch down by ln(1 + (size / COLLISION_SIZE_FOR_STANDARD_PITCH)) / ln(2)
    const float COLLISION_SIZE_FOR_STANDARD_PITCH = 0.2f;
    const float stretchFactor = logf(1.0f + (minAACube.getLargestDimension() / COLLISION_SIZE_FOR_STANDARD_PITCH)) / logf(2.0f);
    AudioInjector::playSound(collisionSound, volume, stretchFactor, collision.contactPoint);
}

void EntityTreeRenderer::entityCollisionWithEntity(const EntityItemID& idA, const EntityItemID& idB,
                                                    const Collision& collision) {
    // If we don't have a tree, or we're in the process of shutting down, then don't
    // process these events.
    if (!_tree || _shuttingDown) {
        return;
    }

    EntityTreePointer entityTree = std::static_pointer_cast<EntityTree>(_tree);
    const QUuid& myNodeID = DependencyManager::get<NodeList>()->getSessionUUID();

    // trigger scripted collision sounds and events for locally owned objects
    EntityItemPointer entityA = entityTree->findEntityByEntityItemID(idA);
    EntityItemPointer entityB = entityTree->findEntityByEntityItemID(idB);
    if ((bool)entityA && (bool)entityB) {
        QUuid entityASimulatorID = entityA->getSimulatorID();
        QUuid entityBSimulatorID = entityB->getSimulatorID();
        bool entityAIsDynamic = entityA->getDynamic();
        bool entityBIsDynamic = entityB->getDynamic();

#ifdef WANT_DEBUG
        bool bothEntitiesStatic = !entityAIsDynamic && !entityBIsDynamic;
        if (bothEntitiesStatic) {
            qCDebug(entities) << "A collision has occurred between two static entities!";
            qCDebug(entities) << "Entity A ID:" << entityA->getID();
            qCDebug(entities) << "Entity B ID:" << entityB->getID();
        }
        assert(!bothEntitiesStatic);
#endif

        if ((myNodeID == entityASimulatorID && entityAIsDynamic) || (myNodeID == entityBSimulatorID && (!entityAIsDynamic || entityASimulatorID.isNull()))) {
            playEntityCollisionSound(entityA, collision);
            emit collisionWithEntity(idA, idB, collision);
            if (_entitiesScriptEngine) {
                _entitiesScriptEngine->callEntityScriptMethod(idA, "collisionWithEntity", idB, collision);
            }
        }

        if ((myNodeID == entityBSimulatorID && entityBIsDynamic) || (myNodeID == entityASimulatorID && (!entityBIsDynamic || entityBSimulatorID.isNull()))) {
            playEntityCollisionSound(entityB, collision);
            // since we're swapping A and B we need to send the inverted collision
            Collision invertedCollision(collision);
            invertedCollision.invert();
            emit collisionWithEntity(idB, idA, invertedCollision);
            if (_entitiesScriptEngine) {
                _entitiesScriptEngine->callEntityScriptMethod(idB, "collisionWithEntity", idA, invertedCollision);
            }
        }
    }
}

void EntityTreeRenderer::updateEntityRenderStatus(bool shouldRenderEntities) {
    if (DependencyManager::get<SceneScriptingInterface>()->shouldRenderEntities()) {
        for (auto entityID : _entityIDsLastInScene) {
            addingEntity(entityID);
        }
        _entityIDsLastInScene.clear();
    } else {
        _entityIDsLastInScene = _entitiesInScene.keys();
        for (auto entityID : _entityIDsLastInScene) {
            // FIXME - is this really right? do we want to do the deletingEntity() code or just remove from the scene.
            deletingEntity(entityID);
        }
    }
}

void EntityTreeRenderer::updateZone(const EntityItemID& id) {
    // Get in the zone!
    auto zone = std::dynamic_pointer_cast<ZoneEntityItem>(getTree()->findEntityByEntityItemID(id));
    if (zone && zone->contains(_avatarPosition)) {
        _layeredZones.update(zone);
    }
}

EntityTreeRenderer::LayeredZones::LayeredZones(LayeredZones&& other) {
    // In a swap:
    // > All iterators and references remain valid. The past-the-end iterator is invalidated.
    bool isSkyboxLayerValid = (other._skyboxLayer != other.end());

    swap(other);
    _map.swap(other._map);
    _skyboxLayer = other._skyboxLayer;

    if (!isSkyboxLayerValid) {
        _skyboxLayer = end();
    }
}

void EntityTreeRenderer::LayeredZones::clear() {
    std::set<LayeredZone>::clear();
    _map.clear();
    _skyboxLayer = end();
}

std::pair<EntityTreeRenderer::LayeredZones::iterator, bool> EntityTreeRenderer::LayeredZones::insert(const LayeredZone& layer) {
    iterator it;
    bool success;
    std::tie(it, success) = std::set<LayeredZone>::insert(layer);

    if (success) {
        _map.emplace(it->id, it);
    }

    return { it, success };
}

void EntityTreeRenderer::LayeredZones::apply() {
    assert(_entityTreeRenderer);
}

void EntityTreeRenderer::LayeredZones::update(std::shared_ptr<ZoneEntityItem> zone) {
    assert(_entityTreeRenderer);
    bool isVisible = zone->isVisible();

    if (empty() && isVisible) {
        // there are no zones: set this one
        insert(zone);
        apply();
        return;
    } else {
        LayeredZone zoneLayer(zone);

        // find this zone's layer, if it exists
        iterator layer = end();
        auto it = _map.find(zoneLayer.id);
        if (it != _map.end()) {
            layer = it->second;
            // if the volume changed, we need to resort the layer (reinsertion)
            // if the visibility changed, we need to erase the layer
            if (zoneLayer.volume != layer->volume || !isVisible) {
                erase(layer);
                _map.erase(it);
                layer = end();
            }
        }

        // (re)insert this zone's layer if necessary
        if (layer == end() && isVisible) {
            std::tie(layer, std::ignore) = insert(zoneLayer);
            _map.emplace(layer->id, layer);
        }
    }
}

bool EntityTreeRenderer::LayeredZones::contains(const LayeredZones& other) {
    bool result = std::equal(other.begin(), other._skyboxLayer, begin());
    if (result) {
        // if valid, set the _skyboxLayer from the other LayeredZones
        _skyboxLayer = std::next(begin(), std::distance(other.begin(), other._skyboxLayer));
    }
    return result;
}

