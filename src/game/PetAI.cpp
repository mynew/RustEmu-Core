/*
 * Copyright (C) 2005-2010 MaNGOS <http://getmangos.com/>
 * Copyright (C) 2011-2012 /dev/rsa for MangosR2 <http://github.com/MangosR2>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "PetAI.h"
#include "Errors.h"
#include "Pet.h"
#include "Player.h"
#include "DBCStores.h"
#include "Spell.h"
#include "ObjectAccessor.h"
#include "SpellMgr.h"
#include "Creature.h"
#include "World.h"
#include "Util.h"

int PetAI::Permissible(const Creature *creature)
{
    if (creature->IsPet())
        return PERMIT_BASE_SPECIAL;

    return PERMIT_BASE_NO;
}

PetAI::PetAI(Creature* c) : CreatureAI(c), inCombat(false)
{
    Reset();
}

void PetAI::Reset()
{
    m_primaryTargetGuid.Clear();
    m_savedTargetGuid.Clear();
    m_attackDistanceRecheckTimer.SetInterval(TIME_INTERVAL_LOOK);
    m_attackDistanceRecheckTimer.Reset();
    m_updateAlliesTimer.SetInterval(ALLIES_UPDATE_TIME);
    m_updateAlliesTimer.Reset();
    UpdateAllies();

    for (uint8 i = PET_SPELL_PASSIVE; i < PET_SPELL_MAX; ++i)
        m_spellType[i].clear();

    m_AIType = PET_AI_PASSIVE;
    m_attackDistance = 0.0f;
    float f_range    = 0.0f;

    m_fMaxRadiusToOwner = std::min(100.0f, m_creature->GetMap()->GetVisibilityDistance() - 10.0f);

    if (!m_creature->GetCharmInfo())
        return;

    uint32 spellsSize = m_creature->IsPet() ? ((Pet*)m_creature)->GetPetAutoSpellSize() : m_creature->GetPetAutoSpellSize();
    uint8 rangedDamageSpells = 0;
    uint8 meleeDamageSpells = 0;

    // classification for pet spells
    for (uint32 i = 0; i < spellsSize; ++i)
    {
        uint32 spellID = m_creature->IsPet() ? ((Pet*)m_creature)->GetPetAutoSpellOnPos(i) : m_creature->GetPetAutoSpellOnPos(i);
        if (!spellID)
            continue;

        SpellEntry const* spellInfo = sSpellStore.LookupEntry(spellID);
        if (!spellInfo)
            continue;

        if (IsPassiveSpell(spellInfo))
        {
            m_spellType[PET_SPELL_PASSIVE].insert(spellID);
            continue;
        }

        if (IsNonCombatSpell(spellInfo))
        {
            // Voidwalker Consume Shadows
            if (IsChanneledSpell(spellInfo))
                m_spellType[PET_SPELL_HEAL].insert(spellID);
            else
                m_spellType[PET_SPELL_NONCOMBAT].insert(spellID);
            continue;
        }

        // need more correct define this type
        if (IsSpellReduceThreat(spellInfo) || IsChanneledSpell(spellInfo))
        {
            m_spellType[PET_SPELL_DEFENCE].insert(spellID);
            continue;
        }

        // Voracious Appetite && Cannibalize && Carrion Feeder
        if (spellInfo->HasAttribute(SPELL_ATTR_ABILITY) && spellInfo->HasAttribute(SPELL_ATTR_EX2_ALLOW_DEAD_TARGET))
        {
            m_spellType[PET_SPELL_HEAL].insert(spellID);
            continue;
        }

        if (IsPositiveSpell(spellInfo) && IsSpellAppliesAura(spellInfo))
        {
            m_spellType[PET_SPELL_BUFF].insert(spellID);
            continue;
        }

        if (spellInfo->HasAttribute(SPELL_ATTR_EX_DISPEL_AURAS_ON_IMMUNITY))
        {
            m_spellType[PET_SPELL_FREEACTION].insert(spellID);
            continue;
        }

        // don't have SPELL_ATTR_EX_DISPEL_AURAS_ON_IMMUNITY !
        if (spellInfo->HasAttribute(SPELL_ATTR_EX_CANT_REFLECTED) ||
            spellInfo->HasAttribute(SPELL_ATTR_EX7_HAS_CHARGE_EFFECT))
        {
            m_spellType[PET_SPELL_ATTACKSTART].insert(spellID);
            continue;
        }

        if (IsSpellIncreaseThreat(spellInfo))
        {
            m_spellType[PET_SPELL_THREAT].insert(spellID);
            continue;
        }

        // all non-combat spells classified.
        switch (spellInfo->GetRangeIndex())
        {
            case SPELL_RANGE_IDX_COMBAT:
            {
                if (IsSpellCauseDamage(spellInfo))
                {
                    m_spellType[PET_SPELL_MELEE].insert(spellID);
                    ++meleeDamageSpells;
                }
                else
                {
                    m_spellType[PET_SPELL_SPECIAL].insert(spellID);
                }
                break;
            }
            // possible debuffs or auras?
            case SPELL_RANGE_IDX_SELF_ONLY:
            case SPELL_RANGE_IDX_ANYWHERE:
            {
                m_spellType[PET_SPELL_SPECIAL].insert(spellID);
                break;
            }
            default:
            {
                float range = GetSpellMaxRange(sSpellRangeStore.LookupEntry(spellInfo->GetRangeIndex()), false);
                if (f_range < M_NULL_F || (range > M_NULL_F && range < f_range))
                    f_range = range;
                if (IsSpellCauseDamage(spellInfo))
                {
                    m_spellType[PET_SPELL_RANGED].insert(spellID);
                    ++rangedDamageSpells;
                }
                else
                {
                    m_spellType[PET_SPELL_SPECIAL].insert(spellID);
                }
                break;
            }
        }
    }

    // define initial AI type
    if (m_creature->IsVehicle())
        m_AIType = PET_AI_PASSIVE;

    if (m_spellType[PET_SPELL_RANGED].size() > 0 && (m_spellType[PET_SPELL_MELEE].size() < m_spellType[PET_SPELL_RANGED].size()))
    {
        m_AIType = PET_AI_RANGED;
        m_attackDistance = f_range - m_creature->GetObjectBoundingRadius() - 2.0f;
        if (m_attackDistance < 20.0f)
            m_attackDistance = 18.0f;
    }
    else
    {
        m_AIType = PET_AI_MELEE;
        m_attackDistance = 0.0f;
    }
    m_savedAIType = m_AIType;

    m_creature->GetMotionMaster()->MoveTargetedHome();

    DEBUG_FILTER_LOG(LOG_FILTER_AI_AND_MOVEGENSS,"PetAI::Reset %s, AI %u dist %f, spells: " SIZEFMTD " " SIZEFMTD " " SIZEFMTD " " SIZEFMTD " " SIZEFMTD " " SIZEFMTD " " SIZEFMTD " " SIZEFMTD " " SIZEFMTD " " SIZEFMTD " " SIZEFMTD " " SIZEFMTD ,
        m_creature->GetObjectGuid().GetString().c_str(),
        m_AIType,
        m_attackDistance,
        m_spellType[PET_SPELL_PASSIVE].size(),
        m_spellType[PET_SPELL_NONCOMBAT].size(),
        m_spellType[PET_SPELL_BUFF].size(),
        m_spellType[PET_SPELL_DEBUFF].size(),
        m_spellType[PET_SPELL_FREEACTION].size(),
        m_spellType[PET_SPELL_ATTACKSTART].size(),
        m_spellType[PET_SPELL_THREAT].size(),
        m_spellType[PET_SPELL_MELEE].size(),
        m_spellType[PET_SPELL_RANGED].size(),
        m_spellType[PET_SPELL_DEFENCE].size(),
        m_spellType[PET_SPELL_SPECIAL].size(),
        m_spellType[PET_SPELL_HEAL].size()
        );
}

void PetAI::MoveInLineOfSight(Unit* pWho)
{
    if (m_creature->hasUnitState(UNIT_STAT_CAN_NOT_REACT))
        return;

    if (m_creature->getVictim())
        return;

    if (m_savedTargetGuid && m_savedTargetGuid == pWho->GetObjectGuid())
        return;

    CharmInfo* charmInfo = m_creature->GetCharmInfo();
    if (!charmInfo)
        return;

    if (m_creature->IsPet() && charmInfo->HasState(CHARM_STATE_ACTION, ACTIONS_DISABLE))
        return;

    if (!charmInfo->HasState(CHARM_STATE_REACT, REACT_AGGRESSIVE))
        return;

    if (!pWho->isTargetableForAttack() || !m_creature->IsHostileTo(pWho) ||
        !pWho->isInAccessablePlaceFor(m_creature) ||
        !pWho->isVisibleForOrDetect(m_creature, m_creature, true))
        return;

    float attackRadius = m_creature->GetAttackDistance(pWho);
    if (m_creature->IsWithinDistInMap(pWho, attackRadius) && m_creature->GetDistanceZ(pWho) <= CREATURE_Z_ATTACK_RANGE)
        AttackStart(pWho);
}

void PetAI::MovementInform(uint32 uiMovementType, uint32 /*uiData*/)
{
    // restore MGen after jumps
    if (uiMovementType != EFFECT_MOTION_TYPE)
        return;

    if (!m_creature->GetCharmInfo() ||
        m_creature->GetCharmInfo()->HasState(CHARM_STATE_ACTION, ACTIONS_DISABLE))
        return;

    if (Unit* pVictim = m_creature->getVictim())
        MoveToVictim(pVictim);
    else
        m_creature->GetMotionMaster()->MoveTargetedHome();
}

void PetAI::AttackStart(Unit* pTarget)
{
    m_savedTargetGuid.Clear();

    if (!pTarget || (m_creature->IsPet() && ((Pet*)m_creature)->getPetType() == MINI_PET))
        return;

    m_creature->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_ENTER_COMBAT);

    if (!pTarget->isVisibleForOrDetect(m_creature, m_creature, true))
        return;

    Unit* owner = m_creature->GetCharmerOrOwner();
    if (owner && !pTarget->isVisibleForOrDetect(owner, owner, true))
        return;

    if (m_creature->Attack(pTarget, m_AIType != PET_AI_RANGED))
    {
        inCombat = true;
        if (!UpdateAIType())
            MoveToVictim(pTarget);
    }
}

bool PetAI::UpdateAIType()
{
    if (!inCombat && m_AIType != PET_AI_PASSIVE)
    {
        m_savedAIType = m_AIType;
        m_AIType = PET_AI_PASSIVE;
        return true;
    }
    else if (inCombat && m_AIType == PET_AI_PASSIVE)
    {
        m_AIType = m_savedAIType;
    }

    if (!inCombat)
        return false;

    if (m_creature->IsVehicle() && m_creature->GetCharmerOrOwner())
    {
        m_AIType = PET_AI_PASSIVE;
        return false;
    }

    if (sWorld.getConfig(CONFIG_BOOL_PET_ADVANCED_AI_SLACKER) &&
        m_AIType != PET_AI_SLACKER &&
        !m_creature->GetCharmInfo()->HasState(CHARM_STATE_REACT, REACT_AGGRESSIVE) &&
        m_creature->HasAuraState(AURA_STATE_HEALTHLESS_20_PERCENT))
    {
        m_savedAIType = m_AIType;
        m_AIType = PET_AI_SLACKER;
        MoveToVictim(m_creature->getVictim());
        return true;
    }
    else if (sWorld.getConfig(CONFIG_BOOL_PET_ADVANCED_AI_SLACKER) &&
        m_AIType == PET_AI_SLACKER &&
        (!m_creature->HasAuraState(AURA_STATE_HEALTHLESS_20_PERCENT) ||
        m_creature->GetCharmInfo()->HasState(CHARM_STATE_REACT, REACT_AGGRESSIVE)))
    {
        m_AIType = m_savedAIType;
        m_creature->GetMotionMaster()->MoveIdle();
        MoveToVictim(m_creature->getVictim());
        return true;
    }

    if (m_AIType == PET_AI_RANGED)
    {
        if (m_creature->GetPower(POWER_MANA) < m_creature->GetMaxPower(POWER_MANA) / 10)
        {
            m_AIType = PET_AI_RANGED_NOAMMO;
            MoveToVictim(m_creature->getVictim());
            return true;
        }
    }
    else if (m_AIType == PET_AI_RANGED_NOAMMO)
    {
        if (m_creature->GetPower(POWER_MANA) > m_creature->GetMaxPower(POWER_MANA) / 4)
        {
            m_AIType = PET_AI_RANGED;
            MoveToVictim(m_creature->getVictim());
            return true;
        }
    }
    return false;
}

void PetAI::MoveToVictim(Unit* pTarget)
{
    if (!pTarget)
        return;

    if (m_creature->IsNonMeleeSpellCasted(false))
        return;

    switch (m_AIType)
    {
        case PET_AI_PASSIVE:
        case PET_AI_SLACKER:
        case PET_AI_HEALER:
            m_creature->GetMotionMaster()->MoveTargetedHome();
            break;
        case PET_AI_RANGED:
            if (sWorld.getConfig(CONFIG_BOOL_PET_ADVANCED_AI))
                m_creature->GetMotionMaster()->MoveChase(pTarget, m_attackDistance, m_creature->GetAngle(pTarget) + frand(-M_PI_F / 4.0f, M_PI_F / 4.0f));
            else
                m_creature->GetMotionMaster()->MoveChase(pTarget);
            break;
        case PET_AI_MELEE:
        case PET_AI_RANGED_NOAMMO:
        default:
            m_creature->GetMotionMaster()->MoveChase(pTarget);
            break;
    }
    DEBUG_FILTER_LOG(LOG_FILTER_AI_AND_MOVEGENSS,"PetAI::MoveToVictim pet %s move to %s, distance %f,  AI type %u",
        m_creature->GetGuidStr().c_str(), pTarget ? pTarget->GetGuidStr().c_str() : "<none>", m_attackDistance, m_AIType);
}

void PetAI::EnterEvadeMode()
{
    Reset();
    UpdateAIType();

    if (!m_creature->IsInUnitState(UNIT_ACTION_HOME))
        m_creature->GetMotionMaster()->MoveTargetedHome();
}

bool PetAI::IsVisible(Unit* pl) const
{
    return _isVisible(pl);
}

bool PetAI::_needToStop() const
{
    Unit* pVictim = m_creature->getVictim();
    if (!pVictim)
        return true;

    // This is needed for charmed creatures, as once their target was reset other effects can trigger threat
    if (m_creature->isCharmed() && m_creature->GetCharmer() == pVictim)
        return true;

    Unit* owner = m_creature->GetCharmerOrOwner();

    if (owner == pVictim)
        return true;

    if (!pVictim->isVisibleForOrDetect(m_creature, m_creature, false))
        return true;

    if (owner && !pVictim->isVisibleForOrDetect(owner, owner, true))
        return true;

    if (m_primaryTargetGuid && m_primaryTargetGuid != pVictim->GetObjectGuid())
        return true;

    return !pVictim->isTargetableForAttack();
}

void PetAI::_stopAttack()
{
    if (!IsInCombat())
        return;

    inCombat = false;

    m_creature->CastStop();
    m_creature->AttackStop();
    m_creature->GetMotionMaster()->Clear(); // move idle

    if (!m_savedTargetGuid)
        m_creature->GetMotionMaster()->MoveTargetedHome();
}

void PetAI::UpdateAI(const uint32 diff)
{
    if (!m_creature->isAlive())
        return;

    m_updateAlliesTimer.Update(diff);
    if (m_updateAlliesTimer.Passed())
    {
        UpdateAllies();
        m_updateAlliesTimer.Reset();
    }

    Unit* owner = m_creature->GetCharmerOrOwner();

    if (owner && !m_creature->IsWithinDistInMap(owner, m_fMaxRadiusToOwner) && !m_creature->IsInUnitState(UNIT_ACTION_HOME))
    {
        if (owner->GetTypeId() == TYPEID_PLAYER && (m_creature->IsPet() || m_creature->isCharmed()))
        {
            owner->CallForAllControlledUnits(DoPetActionWithHelper((Player*)owner, ACT_REACTION, REACT_PASSIVE, m_creature->GetObjectGuid(), ObjectGuid()), CONTROLLED_PET | CONTROLLED_GUARDIANS | CONTROLLED_CHARM);
            owner->CallForAllControlledUnits(DoPetActionWithHelper((Player*)owner, ACT_COMMAND, COMMAND_FOLLOW, m_creature->GetObjectGuid(), ObjectGuid()), CONTROLLED_PET | CONTROLLED_GUARDIANS | CONTROLLED_CHARM);
            return;
        }
    }

    if (!inCombat && m_savedTargetGuid)
    {
        if (Unit* savedTarget = m_creature->GetMap()->GetUnit(m_savedTargetGuid))
        {
            if (!savedTarget->isAlive())
                m_savedTargetGuid.Clear();
            else if (!savedTarget->IsCrowdControlled())
                AttackStart(savedTarget);
        }
        else
            m_savedTargetGuid.Clear();
    }

    Unit* pVictim = m_creature->getVictim();

    if (inCombat &&
        (!pVictim || !pVictim->isAlive() ||
        (m_creature->IsPet() && m_creature->GetCharmInfo()->HasState(CHARM_STATE_ACTION, ACTIONS_DISABLE))))
        _stopAttack();

    if (m_creature->hasUnitState(UNIT_STAT_CAN_NOT_REACT) || m_creature->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_PACIFIED))
    {
        UpdateAIType();
        return;
    }

    // i_pet.getVictim() can't be used for check in case stop fighting, i_pet.getVictim() clear at Unit death etc.
    if (pVictim)
    {
        bool meleeReach = m_creature->CanReachWithMeleeAttack(pVictim);

        if (_needToStop())
        {
            DEBUG_FILTER_LOG(LOG_FILTER_AI_AND_MOVEGENSS, "PetAI (guid = %u) is stopping attack.", m_creature->GetGUIDLow());
            _stopAttack();
            return;
        }
        else if (!pVictim->isAlive())                        // Stop attack if target dead
        {
            m_creature->InterruptNonMeleeSpells(false);
            _stopAttack();
            return;
        }
        // Stop attack if target under CC effect
        else if (sWorld.getConfig(CONFIG_BOOL_PET_ADVANCED_AI) && IsInCombat() &&
            pVictim->IsCrowdControlled() &&
            !m_creature->GetCurrentSpell(CURRENT_CHANNELED_SPELL))
        {
            m_savedTargetGuid = pVictim->GetObjectGuid();
            m_creature->InterruptSpell(CURRENT_GENERIC_SPELL, true);
            if (!m_creature->IsNonMeleeSpellCasted(false, false, true))
                _stopAttack();
            return;
        }
        else if (m_creature->IsStopped() || meleeReach)
        {
            // required to be stopped cases
            if (m_creature->IsStopped() && m_creature->IsNonMeleeSpellCasted(false))
            {
                if (m_creature->hasUnitState(UNIT_STAT_FOLLOW_MOVE))
                    m_creature->InterruptNonMeleeSpells(false);
                else
                    return;
            }
            // not required to be stopped case
            else if (DoMeleeAttackIfReady())
            {
                pVictim = m_creature->getVictim();
                if (!pVictim)
                    return;

                // if pet misses its target, it will also be the first in threat list
                pVictim->AddThreat(m_creature);

                if (_needToStop())
                    _stopAttack();
            }
        }

        if (!m_creature->IsNonMeleeSpellCasted(true))
        {
            m_attackDistanceRecheckTimer.Update(diff);
            if (m_attackDistanceRecheckTimer.Passed())
            {
                m_attackDistanceRecheckTimer.Reset();
                if (sWorld.getConfig(CONFIG_BOOL_PET_ADVANCED_AI) && m_AIType == PET_AI_RANGED && pVictim)
                {
                    float dist = m_creature->GetDistance(pVictim);
                    if ((m_creature->CanReachWithMeleeAttack(pVictim) &&
                        m_creature->IsWithinDist(owner, m_creature->GetMap()->GetVisibilityDistance() / 2.0f)) ||
                        dist > (m_attackDistance + 2.0f))
                    {
                        MoveToVictim(pVictim);
                        return;
                    }
                }

                if (sWorld.getConfig(CONFIG_BOOL_PET_ADVANCED_AI))
                {
                    // FIXME: AOE check
                }
            }
        }
    }
    else if (Unit* target = GetPrimaryTarget())
    {
        AttackStart(target);
    }
    else if (owner)
    {
        switch (m_creature->GetCharmState(CHARM_STATE_REACT))
        {
            case REACT_DEFENSIVE:
            {
                if (!m_primaryTargetGuid)
                {
                    Unit* ownerVictim = owner->getVictim();
                    if (ownerVictim && ownerVictim->isAlive())
                        AttackStart(ownerVictim);
                }
                break;
            }
            case REACT_AGGRESSIVE:
            {
                if (Unit* pTarget = owner->getAttackerForHelper())
                    AttackStart(pTarget);
                break;
            }
            // case REACT_PASSIVE:
            default:
                break;
        }
    }

    UpdateAIType();

    if (m_creature->IsNonMeleeSpellCasted(true))
        return;

    // Autocast (casted only in combat or persistent spells in any state)
    if (!sWorld.getConfig(CONFIG_BOOL_PET_ADVANCED_AI) && m_AIType != PET_AI_PASSIVE)
    {
        typedef std::vector<std::pair<ObjectGuid, uint32> > TargetSpellList;
        TargetSpellList targetSpellStore;
        
        for (uint8 i = 0; i < m_creature->GetPetAutoSpellSize(); ++i)
        {
            uint32 spellID = m_creature->GetPetAutoSpellOnPos(i);
            if (!spellID)
                continue;

            SpellEntry const* spellInfo = sSpellStore.LookupEntry(spellID);
            if (!spellInfo)
                continue;

            if (m_creature->GetCharmInfo() && m_creature->GetCharmInfo()->GetGlobalCooldownMgr().HasGlobalCooldown(spellInfo))
                continue;

            if (m_creature->HasSpellCooldown(spellInfo))
                continue;
            
            // ignore some combinations of combat state and combat/noncombat spells
            if (!inCombat)
            {
                // ignore attacking spells, and allow only self/around spells
                if (!IsPositiveSpell(spellInfo->Id))
                    continue;

                // non combat spells allowed
                // only pet spells have IsNonCombatSpell and not fit this reqs:
                // Consume Shadows, Lesser Invisibility, so ignore checks for its
                if (!IsNonCombatSpell(spellInfo))
                {
                    // allow only spell without spell cost or with spell cost but not duration limit
                    int32 duration = GetSpellDuration(spellInfo);
                    if ((spellInfo->manaCost || spellInfo->ManaCostPercentage || spellInfo->manaPerSecond) && duration > 0)
                        continue;

                    // allow only spell without cooldown > duration
                    int32 cooldown = GetSpellRecoveryTime(spellInfo);
                    if (cooldown >= 0 && duration >= 0 && cooldown > duration)
                        continue;
                }
            }
            else
            {
                // just ignore non-combat spells
                if (IsNonCombatSpell(spellInfo))
                    continue;
            }

            Unit* autoCastTarget = NULL;

            if (inCombat)
            {
                Unit* pVictim = m_creature->getVictim();
                if (pVictim && !m_creature->hasUnitState(UNIT_STAT_FOLLOW))
                {
                    SpellCastResult result = CanAutoCast(pVictim, spellInfo);
                    if (result == SPELL_CAST_OK || result == SPELL_FAILED_UNIT_NOT_INFRONT)
                        autoCastTarget = pVictim;
                }
            }

            if (!autoCastTarget)
            {
                for (GuidSet::const_iterator tar = m_AllySet.begin(); tar != m_AllySet.end(); ++tar)
                {
                    Unit* target = m_creature->GetMap()->GetUnit(*tar);

                    // Only buff targets that are in combat, unless the spell can only be cast while out of combat
                    if (!target)
                        continue;

                    SpellCastResult result = CanAutoCast(target, spellInfo);
                    if (result == SPELL_CAST_OK || result == SPELL_FAILED_UNIT_NOT_INFRONT)
                    {
                        autoCastTarget = target;
                        break;
                    }
                }
            }

            if (autoCastTarget)
                targetSpellStore.push_back(TargetSpellList::value_type(autoCastTarget->GetObjectGuid(), spellInfo->Id));
        }

        // found units to cast on to
        if (!targetSpellStore.empty())
        {
            uint32 index = urand(0, targetSpellStore.size() - 1);
            if (Unit* target = m_creature->GetMap()->GetUnit(targetSpellStore[index].first))
                m_creature->DoPetCastSpell(target, targetSpellStore[index].second);
        }
    }
    else
    {
        AutoSpellList currentSpells;
        switch (m_AIType)
        {
            case PET_AI_PASSIVE:
            {
                currentSpells.push_back(GetSpellType(PET_SPELL_BUFF));
                break;
            }
            case PET_AI_SLACKER:
            {
                if (!IsInCombat())
                    break;

                if (m_creature->IsCrowdControlled() || (owner && owner->IsCrowdControlled()))
                    currentSpells.push_back(GetSpellType(PET_SPELL_FREEACTION));
                currentSpells.push_back(GetSpellType(PET_SPELL_DEFENCE));
                currentSpells.push_back(GetSpellType(PET_SPELL_BUFF));
                currentSpells.push_back(GetSpellType(PET_SPELL_DEBUFF));
                currentSpells.push_back(GetSpellType(PET_SPELL_RANGED));
                break;
            }
            case PET_AI_HEALER:
            {
                if (!IsInCombat())
                    break;

                if (m_creature->IsCrowdControlled() || (owner && owner->IsCrowdControlled()))
                    currentSpells.push_back(GetSpellType(PET_SPELL_FREEACTION));
                if (m_creature->GetHealth() < m_creature->GetMaxHealth() ||
                    (owner && (owner->GetHealth() < owner->GetMaxHealth())))
                    currentSpells.push_back(GetSpellType(PET_SPELL_HEAL));
                currentSpells.push_back(GetSpellType(PET_SPELL_BUFF));
                currentSpells.push_back(GetSpellType(PET_SPELL_RANGED));
                break;
            }
            case PET_AI_RANGED:
            {
                if (!IsInCombat())
                    break;

                if (m_creature->IsCrowdControlled() || (owner && owner->IsCrowdControlled()))
                    currentSpells.push_back(GetSpellType(PET_SPELL_FREEACTION));
                currentSpells.push_back(GetSpellType(PET_SPELL_RANGED));
                currentSpells.push_back(GetSpellType(PET_SPELL_DEBUFF));
                currentSpells.push_back(GetSpellType(PET_SPELL_BUFF));
                break;
            }
            case PET_AI_MELEE:
            case PET_AI_RANGED_NOAMMO:
            {
                if (!IsInCombat())
                    break;

                if (Unit* victim = m_creature->getVictim())
                {
                    Unit* victimVictim = victim->getVictim();
                    if (!victimVictim || (victimVictim->GetObjectGuid() != m_creature->GetObjectGuid()))
                    {
                        currentSpells.push_back(GetSpellType(PET_SPELL_ATTACKSTART));
                        currentSpells.push_back(GetSpellType(PET_SPELL_THREAT));
                    }
                }

                if (m_creature->IsCrowdControlled() || (owner && owner->IsCrowdControlled()))
                    currentSpells.push_back(GetSpellType(PET_SPELL_FREEACTION));
            }
            /* no break here!*/
            default:
            {
                if (!IsInCombat())
                    break;

                currentSpells.push_back(GetSpellType(PET_SPELL_MELEE));
                currentSpells.push_back(GetSpellType(PET_SPELL_DEBUFF));
                currentSpells.push_back(GetSpellType(PET_SPELL_RANGED));
                currentSpells.push_back(GetSpellType(PET_SPELL_BUFF));
                break;
            }
        }

        if (!IsInCombat())
        {
            currentSpells.push_back(GetSpellType(PET_SPELL_NONCOMBAT));

            if (m_creature->GetHealthPercent() < 95.0f) {
              const SpellEntry *spellInfo = NULL;
              uint32            spellID   = GetSpellType(PET_SPELL_HEAL);
              bool              stopped   = m_creature->IsStopped( );

              stopped |= ( m_creature->GetMovementInfo( ).HasMovementFlag( movementFlagsMask ) == false );
              
              if ( ( spellInfo = sSpellStore.LookupEntry( spellID ) ) != NULL ) {
                if ( ( IsChanneledSpell( spellInfo ) == false ) || ( stopped == true ) ) {
                  currentSpells.push_back( spellID );
                }
              }
            }
        }
        else
            currentSpells.push_back(GetSpellType(PET_SPELL_SPECIAL));

        for (AutoSpellList::const_iterator itr = currentSpells.begin(); itr != currentSpells.end(); ++itr)
        {
            uint32 spellID = *itr;
            if (!spellID)
                continue;

            SpellEntry const* spellInfo = sSpellStore.LookupEntry(spellID);
            if (!spellInfo)
                continue;

            if (m_creature->GetCharmInfo() && m_creature->GetCharmInfo()->GetGlobalCooldownMgr().HasGlobalCooldown(spellInfo))
                continue;

            Unit* pTarget = m_creature->IsPet()
                ? ((Pet*)m_creature)->SelectPreferredTargetForSpell(spellInfo)
                : ((Creature*)m_creature)->SelectPreferredTargetForSpell(spellInfo);

            bool b_castOk = false;

            if (pTarget)
            {
                SpellCastResult result = CanAutoCast(pTarget, spellInfo);
                DEBUG_FILTER_LOG(LOG_FILTER_AI_AND_MOVEGENSS,"PetAI::Update %s, AI %u try cast %u Target %s",
                            m_creature->GetGuidStr().c_str(),
                            m_AIType,
                            spellID,
                            pTarget ? pTarget->GetGuidStr().c_str() : "<none>");

                switch (result)
                {
                    case SPELL_FAILED_TOO_CLOSE:
                    case SPELL_FAILED_OUT_OF_RANGE:
                        break; // ignore
                    case SPELL_FAILED_UNIT_NOT_INFRONT:
                    {
                        if (DoCastSpellIfCan(pTarget, spellID) == CAST_OK)
                        {
                            b_castOk = true;
                            m_creature->SetInFront(pTarget);
                            if (pTarget->GetTypeId() == TYPEID_PLAYER)
                                m_creature->SendCreateUpdateToPlayer((Player*)pTarget);
                        }
                        break;
                    }
                    case SPELL_CAST_OK:
                    {
                        if (DoCastSpellIfCan(pTarget, spellID) == CAST_OK)
                            b_castOk = true;

                        break;
                    }
                    default:
                    {
                        Player* owner = (Player*)m_creature->GetOwner();
                        if (owner)
                            Spell::SendCastResult(owner, spellInfo, 0, result, true);

                        DEBUG_FILTER_LOG(LOG_FILTER_AI_AND_MOVEGENSS, "PetAI::Update cast %s, AI %u Target %s spell %u result %u",
                            m_creature->GetGuidStr().c_str(),
                            m_AIType,
                            pTarget ? pTarget->GetGuidStr().c_str() : "<none>",
                            spellID,
                            result);
                        break;
                    }
                }
            }
            else
                continue;

            if (b_castOk)
            {
                if (m_creature->IsPet())
                {
                    if (((Pet*)m_creature)->getPetType() == SUMMON_PET && (urand(0, 100) < 10))
                        m_creature->SendPetTalk((uint32)PET_TALK_SPECIAL_SPELL);
                    else
                        m_creature->SendPetAIReaction();
                }
                break;
            }
        }
    }
}

bool PetAI::_isVisible(Unit* u) const
{
    return m_creature->IsWithinDist(u, sWorld.getConfig(CONFIG_FLOAT_SIGHT_GUARDER))
        && u->isVisibleForOrDetect(m_creature, m_creature, true);
}

void PetAI::UpdateAllies()
{
    Unit* owner = m_creature->GetCharmerOrOwner();
    if (!owner)
        return;

    Group* pGroup = NULL;

    if (owner->GetTypeId() == TYPEID_PLAYER)
        pGroup = ((Player*)owner)->GetGroup();

    // only pet and owner/not in group->ok
    if (m_AllySet.size() == 2 && !pGroup)
        return;

    // owner is in group; group members filled in already (no raid -> subgroupcount = whole count)
    if (pGroup && !pGroup->isRaidGroup() && m_AllySet.size() == (pGroup->GetMembersCount() + 2))
        return;

    m_AllySet.clear();
    m_AllySet.insert(m_creature->GetObjectGuid());

    if (pGroup)                                              // add group
    {
        for (GroupReference* itr = pGroup->GetFirstMember(); itr != NULL; itr = itr->next())
        {
            Player* target = itr->getSource();
            if (!target || !pGroup->SameSubGroup((Player*)owner, target))
                continue;

            if (target->GetObjectGuid() == owner->GetObjectGuid())
                continue;

            m_AllySet.insert(target->GetObjectGuid());
        }
    }
    else                                                    // remove group
        m_AllySet.insert(owner->GetObjectGuid());
}

void PetAI::AttackedBy(Unit* attacker)
{
    if (m_AIType == PET_AI_SLACKER)
    {
        // special reaction (like change movement type) here
        return;
    }

    // when attacked, fight back in case 1) no victim already AND 2) not set to passive AND 3) not set to stay, unless can it can reach attacker with melee attack anyway
    if (!m_creature->getVictim() && m_creature->GetCharmInfo() && !m_creature->GetCharmInfo()->HasState(CHARM_STATE_REACT, REACT_PASSIVE) &&
        (!m_creature->GetCharmInfo()->HasState(CHARM_STATE_COMMAND, COMMAND_STAY) || m_creature->CanReachWithMeleeAttack(attacker)))
        AttackStart(attacker);
}

SpellCastResult PetAI::CanAutoCast(Unit* target, SpellEntry const* spellInfo)
{
    if (!spellInfo || !target)
        return SPELL_FAILED_DONT_REPORT;

    Spell spell = Spell(m_creature, spellInfo, false);
    return spell.CanAutoCast(target);
}

uint32 PetAI::GetSpellType(PetAutoSpellType type)
{
    if (type >= PET_SPELL_MAX || m_spellType[type].empty())
        return 0;

    std::vector<uint32> tmpSet;

    for (Unit::SpellIdSet::const_iterator itr = m_spellType[type].begin(); itr != m_spellType[type].end(); ++itr)
    {
        uint32 _spellID = *itr;
        if (!_spellID)
            continue;

        SpellEntry const* spellInfo = sSpellStore.LookupEntry(_spellID);
        if (!spellInfo)
            continue;

        if (m_creature->GetCharmInfo() && m_creature->GetCharmInfo()->GetGlobalCooldownMgr().HasGlobalCooldown(spellInfo))
            continue;

        if (m_creature->HasSpellCooldown(spellInfo))
            continue;

        if (IsInCombat() && IsNonCombatSpell(spellInfo))
            continue;

        if (!IsInCombat() && IsPositiveSpell(spellInfo) && !IsNonCombatSpell(spellInfo))
        {
            int32 duration = GetSpellDuration(spellInfo);
//            if ((spellInfo->manaCost || spellInfo->ManaCostPercentage || spellInfo->manaPerSecond) && duration > 0)
//                continue;

            // allow only spell without cooldown > duration
            int32 cooldown = GetSpellRecoveryTime(spellInfo);
            if (cooldown >= 0 && duration >= 0 && cooldown > duration)
                continue;
        }

        tmpSet.push_back(_spellID);
    }

    if (tmpSet.empty())
        return 0;
    else
        return tmpSet[urand(0, tmpSet.size() - 1)];
}

bool PetAI::IsInCombat()
{
    return (inCombat || m_creature->isInCombat());
}

bool PetAI::SetPrimaryTarget(ObjectGuid const& guid)
{
    if (!guid || (!m_primaryTargetGuid || !GetPrimaryTarget()))
    {
        m_primaryTargetGuid = guid;
        return true;
    }
    return false;
}

Unit* PetAI::GetPrimaryTarget()
{
    if (!m_primaryTargetGuid || !m_primaryTargetGuid.IsUnit())
        return NULL;

    Unit* target = m_creature->GetMap()->GetUnit(m_primaryTargetGuid);

    if (!target || !target->isAlive() || !target->isTargetableForAttack() || !target->isInAccessablePlaceFor(m_creature))
    {
        m_primaryTargetGuid.Clear();
        return NULL;
    }

    return target;
}
