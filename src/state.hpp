#pragma once
// Full save/restore of the game at one physics tick.
//
// GD's own CheckpointObject covers the level (GJGameState, triggers, groups,
// audio state). Its PlayerCheckpoint is lossy, so each player is also copied
// member by member (player_state.hpp, generated from the bindings).

#include <Geode/Geode.hpp>
#include "player_state.hpp"

namespace pf {

struct PlayerState {
    PlayerFields fields;
    cocos2d::CCPoint nodePos;
    float rotX = 0.f, rotY = 0.f, scaleX = 1.f, scaleY = 1.f;
    bool visible = true;
    double posX = 0.0, posY = 0.0;
    float width = 0.f, height = 0.f;
    bool flipX = false, flipY = false;
    std::vector<geode::Ref<cocos2d::CCObject>> touchingRings;

    void save(PlayerObject* p) {
        savePlayerFields(p, fields);
        nodePos = p->getPosition();
        rotX = p->getRotationX();
        rotY = p->getRotationY();
        scaleX = p->getScaleX();
        scaleY = p->getScaleY();
        visible = p->isVisible();
        posX = p->m_positionX;
        posY = p->m_positionY;
        width = p->m_width;
        height = p->m_height;
        flipX = p->m_isFlipX;
        flipY = p->m_isFlipY;
        touchingRings.clear();
        if (p->m_touchingRings) {
            for (auto obj : geode::cocos::CCArrayExt<cocos2d::CCObject*>(p->m_touchingRings))
                touchingRings.emplace_back(obj);
        }
    }

    void load(PlayerObject* p) const {
        p->setPosition(nodePos);
        p->setRotationX(rotX);
        p->setRotationY(rotY);
        p->setScaleX(scaleX);
        p->setScaleY(scaleY);
        p->setVisible(visible);
        loadPlayerFields(p, fields);
        p->m_positionX = posX;
        p->m_positionY = posY;
        p->m_width = width;
        p->m_height = height;
        p->m_isFlipX = flipX;
        p->m_isFlipY = flipY;
        p->m_isObjectRectDirty = true;
        p->m_isOrientedBoxDirty = true;
        if (p->m_touchingRings) {
            p->m_touchingRings->removeAllObjects();
            for (auto const& obj : touchingRings)
                p->m_touchingRings->addObject(obj.data());
        }
    }
};

struct Snapshot {
    int tick = 0;
    geode::Ref<CheckpointObject> checkpoint;
    PlayerState p1, p2;
    uint64_t seed = 0;
};

} // namespace pf
