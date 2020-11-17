/*
 * Souffle - A Datalog Compiler
 * Copyright (c) 2013, 2015, Oracle and/or its affiliates. All rights reserved
 * Licensed under the Universal Permissive License v 1.0 as shown at:
 * - https://opensource.org/licenses/UPL
 * - <souffle root>/licenses/SOUFFLE-UPL.txt
 */

/************************************************************************
 *
 * @file AstToRamTranslator.cpp
 *
 * Translator from AST to RAM structures.
 *
 ***********************************************************************/

#include "ast2ram/AstToRamTranslator.h"
#include "Global.h"
#include "LogStatement.h"
#include "ast/Aggregator.h"
#include "ast/Argument.h"
#include "ast/Atom.h"
#include "ast/BinaryConstraint.h"
#include "ast/BranchInit.h"
#include "ast/Clause.h"
#include "ast/Constant.h"
#include "ast/Directive.h"
#include "ast/Negation.h"
#include "ast/NilConstant.h"
#include "ast/Node.h"
#include "ast/NumericConstant.h"
#include "ast/Program.h"
#include "ast/QualifiedName.h"
#include "ast/RecordInit.h"
#include "ast/Relation.h"
#include "ast/StringConstant.h"
#include "ast/TranslationUnit.h"
#include "ast/analysis/AuxArity.h"
#include "ast/analysis/Functor.h"
#include "ast/analysis/IOType.h"
#include "ast/analysis/PolymorphicObjects.h"
#include "ast/analysis/RecursiveClauses.h"
#include "ast/analysis/RelationDetailCache.h"
#include "ast/analysis/RelationSchedule.h"
#include "ast/analysis/SCCGraph.h"
#include "ast/analysis/SumTypeBranches.h"
#include "ast/analysis/TopologicallySortedSCCGraph.h"
#include "ast/analysis/TypeEnvironment.h"
#include "ast/utility/NodeMapper.h"
#include "ast/utility/SipsMetric.h"
#include "ast/utility/Utils.h"
#include "ast/utility/Visitor.h"
#include "ast2ram/ClauseTranslator.h"
#include "ast2ram/ConstraintTranslator.h"
#include "ast2ram/ValueIndex.h"
#include "ast2ram/ValueTranslator.h"
#include "ast2ram/utility/Utils.h"
#include "ram/Call.h"
#include "ram/Clear.h"
#include "ram/Condition.h"
#include "ram/Conjunction.h"
#include "ram/Constraint.h"
#include "ram/DebugInfo.h"
#include "ram/EmptinessCheck.h"
#include "ram/Exit.h"
#include "ram/Expression.h"
#include "ram/Extend.h"
#include "ram/Filter.h"
#include "ram/FloatConstant.h"
#include "ram/IO.h"
#include "ram/LogRelationTimer.h"
#include "ram/LogSize.h"
#include "ram/LogTimer.h"
#include "ram/Loop.h"
#include "ram/Negation.h"
#include "ram/Parallel.h"
#include "ram/Program.h"
#include "ram/Project.h"
#include "ram/Query.h"
#include "ram/Relation.h"
#include "ram/RelationSize.h"
#include "ram/Scan.h"
#include "ram/Sequence.h"
#include "ram/SignedConstant.h"
#include "ram/Statement.h"
#include "ram/Swap.h"
#include "ram/TranslationUnit.h"
#include "ram/TupleElement.h"
#include "ram/UnsignedConstant.h"
#include "ram/utility/Utils.h"
#include "reports/DebugReport.h"
#include "reports/ErrorReport.h"
#include "souffle/BinaryConstraintOps.h"
#include "souffle/SymbolTable.h"
#include "souffle/TypeAttribute.h"
#include "souffle/utility/ContainerUtil.h"
#include "souffle/utility/FunctionalUtil.h"
#include "souffle/utility/MiscUtil.h"
#include "souffle/utility/StringUtil.h"
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace souffle::ast2ram {

AstToRamTranslator::AstToRamTranslator() = default;
AstToRamTranslator::~AstToRamTranslator() = default;

void AstToRamTranslator::addRamSubroutine(std::string subroutineID, Own<ram::Statement> subroutine) {
    assert(!contains(ramSubroutines, subroutineID) && "subroutine ID should not already exist");
    ramSubroutines[subroutineID] = std::move(subroutine);
}

void AstToRamTranslator::addRamRelation(std::string relationName, Own<ram::Relation> ramRelation) {
    assert(!contains(ramRelations, relationName) && "ram relation should not already exist");
    ramRelations[relationName] = std::move(ramRelation);
}

size_t AstToRamTranslator::getEvaluationArity(const ast::Atom* atom) const {
    std::string relName = atom->getQualifiedName().toString();
    if (isPrefix("@info_", relName)) return 0;

    // Get the original relation name
    if (isPrefix("@delta_", relName)) {
        relName = stripPrefix("@delta_", relName);
    } else if (isPrefix("@new_", relName)) {
        relName = stripPrefix("@new_", relName);
    }

    const auto* originalRelation = relDetail->getRelation(ast::QualifiedName(relName));
    return auxArityAnalysis->getArity(originalRelation);
}

std::vector<std::map<std::string, std::string>> AstToRamTranslator::getInputDirectives(
        const ast::Relation* rel) {
    std::vector<std::map<std::string, std::string>> inputDirectives;
    for (const auto* load : getDirectives(*program, rel->getQualifiedName())) {
        // must be a load
        if (load->getType() != ast::DirectiveType::input) {
            continue;
        }

        std::map<std::string, std::string> directives;
        for (const auto& [key, value] : load->getParameters()) {
            directives.insert(std::make_pair(key, unescape(value)));
        }
        inputDirectives.push_back(directives);
    }

    // add an empty directive if none exist
    if (inputDirectives.empty()) {
        inputDirectives.emplace_back();
    }

    return inputDirectives;
}

std::vector<std::map<std::string, std::string>> AstToRamTranslator::getOutputDirectives(
        const ast::Relation* rel) {
    std::vector<std::map<std::string, std::string>> outputDirectives;
    for (const auto* store : getDirectives(*program, rel->getQualifiedName())) {
        // must be either printsize or output
        if (store->getType() != ast::DirectiveType::printsize &&
                store->getType() != ast::DirectiveType::output) {
            continue;
        }

        std::map<std::string, std::string> directives;
        for (const auto& [key, value] : store->getParameters()) {
            directives.insert(std::make_pair(key, unescape(value)));
        }
        outputDirectives.push_back(directives);
    }

    // add an empty directive if none exist
    if (outputDirectives.empty()) {
        outputDirectives.emplace_back();
    }

    return outputDirectives;
}

Own<ram::Expression> AstToRamTranslator::translateValue(
        const ast::Argument* arg, const ValueIndex& index) const {
    if (arg == nullptr) return nullptr;
    return ValueTranslator::translate(*this, index, *symbolTable, *arg);
}

Own<ram::Condition> AstToRamTranslator::translateConstraint(
        const ast::Literal* lit, const ValueIndex& index) {
    assert(lit != nullptr && "literal should be defined");
    return ConstraintTranslator::translate(*this, index, *lit);
}

RamDomain AstToRamTranslator::getConstantRamRepresentation(const ast::Constant& constant) {
    if (auto strConstant = dynamic_cast<const ast::StringConstant*>(&constant)) {
        return symbolTable->lookup(strConstant->getConstant());
    } else if (isA<ast::NilConstant>(&constant)) {
        return 0;
    } else if (auto* numConstant = dynamic_cast<const ast::NumericConstant*>(&constant)) {
        assert(numConstant->getFinalType().has_value() && "constant should have valid type");
        switch (numConstant->getFinalType().value()) {
            case ast::NumericConstant::Type::Int:
                return RamSignedFromString(numConstant->getConstant(), nullptr, 0);
            case ast::NumericConstant::Type::Uint:
                return RamUnsignedFromString(numConstant->getConstant(), nullptr, 0);
            case ast::NumericConstant::Type::Float: return RamFloatFromString(numConstant->getConstant());
        }
    }

    fatal("unaccounted-for constant");
}

Own<ram::Expression> AstToRamTranslator::translateConstant(ast::Constant const& c) {
    auto const rawConstant = getConstantRamRepresentation(c);
    if (auto* const c_num = dynamic_cast<const ast::NumericConstant*>(&c)) {
        switch (c_num->getFinalType().value()) {
            case ast::NumericConstant::Type::Int: return mk<ram::SignedConstant>(rawConstant);
            case ast::NumericConstant::Type::Uint: return mk<ram::UnsignedConstant>(rawConstant);
            case ast::NumericConstant::Type::Float: return mk<ram::FloatConstant>(rawConstant);
        }
        fatal("unaccounted-for constant");
    }
    return mk<ram::SignedConstant>(rawConstant);
}

/** generate RAM code for a non-recursive relation */
Own<ram::Statement> AstToRamTranslator::translateNonRecursiveRelation(const ast::Relation& rel) {
    /* start with an empty sequence */
    VecOwn<ram::Statement> res;

    std::string relName = getConcreteRelationName(&rel);

    /* iterate over all clauses that belong to the relation */
    for (ast::Clause* clause : relDetail->getClauses(rel.getQualifiedName())) {
        // skip recursive rules
        if (recursiveClauses->recursive(clause)) {
            continue;
        }

        // translate clause
        Own<ram::Statement> rule = ClauseTranslator(*this).translateClause(*clause, *clause);

        // add logging
        if (Global::config().has("profile")) {
            const std::string& relationName = toString(rel.getQualifiedName());
            const auto& srcLocation = clause->getSrcLoc();
            const std::string clauseText = stringify(toString(*clause));
            const std::string logTimerStatement =
                    LogStatement::tNonrecursiveRule(relationName, srcLocation, clauseText);
            const std::string logSizeStatement =
                    LogStatement::nNonrecursiveRule(relationName, srcLocation, clauseText);
            rule = mk<ram::LogRelationTimer>(std::move(rule), logTimerStatement, relName);
        }

        // add debug info
        std::ostringstream ds;
        ds << toString(*clause) << "\nin file ";
        ds << clause->getSrcLoc();
        rule = mk<ram::DebugInfo>(std::move(rule), ds.str());

        // add rule to result
        appendStmt(res, std::move(rule));
    }

    // add logging for entire relation
    if (Global::config().has("profile")) {
        const std::string& relationName = toString(rel.getQualifiedName());
        const auto& srcLocation = rel.getSrcLoc();
        const std::string logSizeStatement = LogStatement::nNonrecursiveRelation(relationName, srcLocation);

        // add timer if we did any work
        if (!res.empty()) {
            const std::string logTimerStatement =
                    LogStatement::tNonrecursiveRelation(relationName, srcLocation);
            auto newStmt =
                    mk<ram::LogRelationTimer>(mk<ram::Sequence>(std::move(res)), logTimerStatement, relName);
            res.clear();
            appendStmt(res, std::move(newStmt));
        } else {
            // add table size printer
            appendStmt(res, mk<ram::LogSize>(relName, logSizeStatement));
        }
    }

    // done
    return mk<ram::Sequence>(std::move(res));
}

Own<ram::Sequence> AstToRamTranslator::translateSCC(size_t scc, size_t idx) {
    // make a new ram statement for the current SCC
    VecOwn<ram::Statement> current;

    // load all internal input relations from the facts dir with a .facts extension
    const auto& sccInputRelations = sccGraph->getInternalInputRelations(scc);
    for (const auto& relation : sccInputRelations) {
        makeRamLoad(current, relation);
    }

    // compute the relations themselves
    const auto& isRecursive = sccGraph->isRecursive(scc);
    const auto& sccRelations = sccGraph->getInternalRelations(scc);
    Own<ram::Statement> bodyStatement =
            (!isRecursive) ? translateNonRecursiveRelation(*((const ast::Relation*)*sccRelations.begin()))
                           : translateRecursiveRelation(sccRelations);
    appendStmt(current, std::move(bodyStatement));

    // store all internal output relations to the output dir with a .csv extension
    const auto& sccOutputRelations = sccGraph->getInternalOutputRelations(scc);
    for (const auto& relation : sccOutputRelations) {
        makeRamStore(current, relation);
    }

    // clear expired relations
    auto clearingStmts = clearExpiredRelations(relationSchedule->schedule().at(idx).expired());
    for (auto& stmt : clearingStmts) {
        appendStmt(current, std::move(stmt));
    }

    return mk<ram::Sequence>(std::move(current));
}

VecOwn<ram::Statement> AstToRamTranslator::clearExpiredRelations(
        const std::set<const ast::Relation*>& expiredRelations) const {
    VecOwn<ram::Statement> stmts;
    for (const auto& relation : expiredRelations) {
        appendStmt(stmts, makeRamClear(relation));
    }
    return stmts;
}

void AstToRamTranslator::addNegation(ast::Clause& clause, const ast::Atom* atom) const {
    if (clause.getHead()->getArity() > 0) {
        clause.addToBody(mk<ast::Negation>(souffle::clone(atom)));
    }
}

Own<ram::Statement> AstToRamTranslator::mergeRelations(
        const ast::Relation* rel, const std::string& destRelation, const std::string& srcRelation) const {
    VecOwn<ram::Expression> values;

    // Proposition - project if not empty
    if (rel->getArity() == 0) {
        auto projection = mk<ram::Project>(destRelation, std::move(values));
        return mk<ram::Query>(mk<ram::Filter>(
                mk<ram::Negation>(mk<ram::EmptinessCheck>(srcRelation)), std::move(projection)));
    }

    // Predicate - project all values
    for (size_t i = 0; i < rel->getArity(); i++) {
        values.push_back(mk<ram::TupleElement>(0, i));
    }
    auto projection = mk<ram::Project>(destRelation, std::move(values));
    auto stmt = mk<ram::Query>(mk<ram::Scan>(srcRelation, 0, std::move(projection)));
    if (rel->getRepresentation() == RelationRepresentation::EQREL) {
        return mk<ram::Sequence>(mk<ram::Extend>(destRelation, srcRelation), std::move(stmt));
    }
    return stmt;
}

VecOwn<ram::Statement> AstToRamTranslator::createRecursiveClauseVersions(
        const std::set<const ast::Relation*>& scc, const ast::Relation* rel) {
    assert(contains(scc, rel) && "relation should belong to scc");
    VecOwn<ram::Statement> loopRelSeq;

    /* Find clauses for relation rel */
    for (const auto& cl : relDetail->getClauses(rel->getQualifiedName())) {
        // skip non-recursive clauses
        if (!recursiveClauses->recursive(cl)) {
            continue;
        }

        // each recursive rule results in several operations
        int version = 0;
        const auto& atoms = ast::getBodyLiterals<ast::Atom>(*cl);
        for (size_t j = 0; j < atoms.size(); ++j) {
            const ast::Atom* atom = atoms[j];
            const ast::Relation* atomRelation = getAtomRelation(atom, program);

            // only interested in atoms within the same SCC
            if (!contains(scc, atomRelation)) {
                continue;
            }

            // modify the processed rule to use delta relation and write to new relation
            auto r1 = souffle::clone(cl);
            r1->getHead()->setQualifiedName(getNewRelationName(rel));
            ast::getBodyLiterals<ast::Atom>(*r1)[j]->setQualifiedName(getDeltaRelationName(atomRelation));
            addNegation(*r1, cl->getHead());

            // replace wildcards with variables to reduce indices
            nameUnnamedVariables(r1.get());

            // reduce R to P ...
            for (size_t k = j + 1; k < atoms.size(); k++) {
                if (contains(scc, getAtomRelation(atoms[k], program))) {
                    auto cur = souffle::clone(ast::getBodyLiterals<ast::Atom>(*r1)[k]);
                    cur->setQualifiedName(getDeltaRelationName(getAtomRelation(atoms[k], program)));
                    r1->addToBody(mk<ast::Negation>(std::move(cur)));
                }
            }

            Own<ram::Statement> rule = ClauseTranslator(*this).translateClause(*r1, *cl, version);

            // add loging
            if (Global::config().has("profile")) {
                const std::string& relationName = toString(rel->getQualifiedName());
                const auto& srcLocation = cl->getSrcLoc();
                const std::string clauseText = stringify(toString(*cl));
                const std::string logTimerStatement =
                        LogStatement::tRecursiveRule(relationName, version, srcLocation, clauseText);
                const std::string logSizeStatement =
                        LogStatement::nRecursiveRule(relationName, version, srcLocation, clauseText);
                rule = mk<ram::LogRelationTimer>(std::move(rule), logTimerStatement, getNewRelationName(rel));
            }

            // add debug info
            std::ostringstream ds;
            ds << toString(*cl) << "\nin file ";
            ds << cl->getSrcLoc();
            rule = mk<ram::DebugInfo>(std::move(rule), ds.str());

            // add to loop body
            appendStmt(loopRelSeq, std::move(rule));

            // increment version counter
            version++;
        }

        // check that the correct number of versions have been created
        if (cl->getExecutionPlan() != nullptr) {
            int maxVersion = -1;
            for (auto const& cur : cl->getExecutionPlan()->getOrders()) {
                maxVersion = std::max(cur.first, maxVersion);
            }
            assert(version > maxVersion && "missing clause versions");
        }
    }

    return loopRelSeq;
}

VecOwn<ram::Statement> AstToRamTranslator::generateStratumPreamble(
        const std::set<const ast::Relation*>& scc) {
    VecOwn<ram::Statement> preamble;
    for (const ast::Relation* rel : scc) {
        // Generate code for the non-recursive part of the relation */
        appendStmt(preamble, translateNonRecursiveRelation(*rel));

        // Copy the result into the delta relation
        appendStmt(preamble, mergeRelations(rel, getDeltaRelationName(rel), getConcreteRelationName(rel)));
    }
    return preamble;
}

VecOwn<ram::Statement> AstToRamTranslator::generateStratumPostamble(
        const std::set<const ast::Relation*>& scc) const {
    VecOwn<ram::Statement> postamble;
    for (const ast::Relation* rel : scc) {
        // Drop temporary tables after recursion
        appendStmt(postamble, mk<ram::Clear>(getDeltaRelationName(rel)));
        appendStmt(postamble, mk<ram::Clear>(getNewRelationName(rel)));
    }
    return postamble;
}

VecOwn<ram::Statement> AstToRamTranslator::generateStratumTableUpdates(
        const std::set<const ast::Relation*>& scc) const {
    VecOwn<ram::Statement> updateTable;
    for (const ast::Relation* rel : scc) {
        // Copy @new into main relation, @delta := @new, and empty out @new
        Own<ram::Statement> updateRelTable =
                mk<ram::Sequence>(mergeRelations(rel, getConcreteRelationName(rel), getNewRelationName(rel)),
                        mk<ram::Swap>(getDeltaRelationName(rel), getNewRelationName(rel)),
                        mk<ram::Clear>(getNewRelationName(rel)));

        // Measure update time
        if (Global::config().has("profile")) {
            updateRelTable = mk<ram::LogRelationTimer>(std::move(updateRelTable),
                    LogStatement::cRecursiveRelation(toString(rel->getQualifiedName()), rel->getSrcLoc()),
                    getNewRelationName(rel));
        }

        appendStmt(updateTable, std::move(updateRelTable));
    }
    return updateTable;
}

VecOwn<ram::Statement> AstToRamTranslator::generateStratumMainLoop(
        const std::set<const ast::Relation*>& scc) {
    VecOwn<ram::Statement> loopSeq;
    for (const ast::Relation* rel : scc) {
        auto loopRelSeq = createRecursiveClauseVersions(scc, rel);

        // if there were no rules, continue
        if (loopRelSeq.empty()) {
            continue;
        }

        // add profiling information
        if (Global::config().has("profile")) {
            const std::string& relationName = toString(rel->getQualifiedName());
            const auto& srcLocation = rel->getSrcLoc();
            const std::string logTimerStatement = LogStatement::tRecursiveRelation(relationName, srcLocation);
            const std::string logSizeStatement = LogStatement::nRecursiveRelation(relationName, srcLocation);
            auto newStmt = mk<ram::LogRelationTimer>(
                    mk<ram::Sequence>(std::move(loopRelSeq)), logTimerStatement, getNewRelationName(rel));
            loopRelSeq.clear();
            appendStmt(loopRelSeq, std::move(newStmt));
        }

        appendStmt(loopSeq, mk<ram::Sequence>(std::move(loopRelSeq)));
    }
    return loopSeq;
}

VecOwn<ram::Statement> AstToRamTranslator::generateStratumExitConditions(
        const std::set<const ast::Relation*>& scc) const {
    // Helper function to add a new term to a conjunctive condition
    auto addCondition = [&](Own<ram::Condition>& cond, Own<ram::Condition> term) {
        cond = (cond == nullptr) ? std::move(term) : mk<ram::Conjunction>(std::move(cond), std::move(term));
    };

    VecOwn<ram::Statement> exitConditions;

    // (1) if all relations in the scc are empty
    Own<ram::Condition> emptinessCheck;
    for (const ast::Relation* rel : scc) {
        addCondition(emptinessCheck, mk<ram::EmptinessCheck>(getNewRelationName(rel)));
    }
    appendStmt(exitConditions, mk<ram::Exit>(std::move(emptinessCheck)));

    // (2) if the size limit has been reached for any limitsize relations
    for (const ast::Relation* rel : scc) {
        if (ioType->isLimitSize(rel)) {
            Own<ram::Condition> limit = mk<ram::Constraint>(BinaryConstraintOp::GE,
                    mk<ram::RelationSize>(getConcreteRelationName(rel)),
                    mk<ram::SignedConstant>(ioType->getLimitSize(rel)));
            appendStmt(exitConditions, mk<ram::Exit>(std::move(limit)));
        }
    }

    return exitConditions;
}

/** generate RAM code for recursive relations in a strongly-connected component */
Own<ram::Statement> AstToRamTranslator::translateRecursiveRelation(
        const std::set<const ast::Relation*>& scc) {
    // -- Initialise all the individual sections --
    auto preamble = generateStratumPreamble(scc);
    auto loopSeq = generateStratumMainLoop(scc);
    auto updateTable = generateStratumTableUpdates(scc);
    auto exitConditions = generateStratumExitConditions(scc);
    auto postamble = generateStratumPostamble(scc);

    // --- Combine the individual sections into the final fixpoint loop --
    VecOwn<ram::Statement> res;

    // Add in the preamble
    if (!preamble.empty()) {
        appendStmt(res, mk<ram::Sequence>(std::move(preamble)));
    }

    // Add in the main loop and update sections
    auto loop = mk<ram::Parallel>(std::move(loopSeq));
    if (!loop->getStatements().empty() && !exitConditions.empty() && !updateTable.empty()) {
        auto ramExitSequence = mk<ram::Sequence>(std::move(exitConditions));
        auto ramUpdateSequence = mk<ram::Sequence>(std::move(updateTable));
        auto ramLoopSequence = mk<ram::Loop>(
                mk<ram::Sequence>(std::move(loop), std::move(ramExitSequence), std::move(ramUpdateSequence)));
        appendStmt(res, std::move(ramLoopSequence));
    }

    // Add in the postamble
    if (!postamble.empty()) {
        appendStmt(res, mk<ram::Sequence>(std::move(postamble)));
    }

    assert(!res.empty() && "not implemented");
    return mk<ram::Sequence>(std::move(res));
}

bool AstToRamTranslator::removeADTs(const ast::TranslationUnit& translationUnit) {
    struct ADTsFuneral : public ast::NodeMapper {
        mutable bool changed{false};
        const ast::analysis::SumTypeBranchesAnalysis& sumTypesBranches;

        ADTsFuneral(const ast::TranslationUnit& tu)
                : sumTypesBranches(*tu.getAnalysis<ast::analysis::SumTypeBranchesAnalysis>()) {}

        Own<ast::Node> operator()(Own<ast::Node> node) const override {
            // Rewrite sub-expressions first
            node->apply(*this);

            if (!isA<ast::BranchInit>(node)) {
                return node;
            }

            changed = true;
            auto& adt = *as<ast::BranchInit>(node);
            auto& type = sumTypesBranches.unsafeGetType(adt.getConstructor());
            auto& branches = type.getBranches();

            // Find branch ID.
            ast::analysis::AlgebraicDataType::Branch searchDummy{adt.getConstructor(), {}};
            auto iterToBranch = std::lower_bound(branches.begin(), branches.end(), searchDummy,
                    [](const ast::analysis::AlgebraicDataType::Branch& left,
                            const ast::analysis::AlgebraicDataType::Branch& right) {
                        return left.name < right.name;
                    });

            // Branch id corresponds to the position in lexicographical ordering.
            auto branchID = std::distance(std::begin(branches), iterToBranch);

            if (isADTEnum(type)) {
                auto branchTag = mk<ast::NumericConstant>(branchID);
                branchTag->setFinalType(ast::NumericConstant::Type::Int);
                return branchTag;
            } else {
                // Collect branch arguments
                VecOwn<ast::Argument> branchArguments;
                for (auto* arg : adt.getArguments()) {
                    branchArguments.emplace_back(arg->clone());
                }

                // Branch is stored either as [branch_id, [arguments]]
                // or [branch_id, argument] in case of a single argument.
                auto branchArgs = [&]() -> Own<ast::Argument> {
                    if (branchArguments.size() != 1) {
                        return mk<ast::Argument, ast::RecordInit>(std::move(branchArguments));
                    } else {
                        return std::move(branchArguments.at(0));
                    }
                }();

                // Arguments for the resulting record [branch_id, branch_args].
                VecOwn<ast::Argument> finalRecordArgs;

                auto branchTag = mk<ast::NumericConstant>(branchID);
                branchTag->setFinalType(ast::NumericConstant::Type::Int);
                finalRecordArgs.push_back(std::move(branchTag));
                finalRecordArgs.push_back(std::move(branchArgs));

                return mk<ast::RecordInit>(std::move(finalRecordArgs), adt.getSrcLoc());
            }
        }
    };

    ADTsFuneral mapper(translationUnit);
    translationUnit.getProgram().apply(mapper);
    return mapper.changed;
}

void AstToRamTranslator::makeRamLoad(VecOwn<ram::Statement>& curStmts, const ast::Relation* relation) {
    for (auto directives : getInputDirectives(relation)) {
        Own<ram::Statement> statement = mk<ram::IO>(getConcreteRelationName(relation), directives);
        if (Global::config().has("profile")) {
            const std::string logTimerStatement = LogStatement::tRelationLoadTime(
                    toString(relation->getQualifiedName()), relation->getSrcLoc());
            statement = mk<ram::LogRelationTimer>(
                    std::move(statement), logTimerStatement, getConcreteRelationName(relation));
        }
        appendStmt(curStmts, std::move(statement));
    }
}

void AstToRamTranslator::makeRamStore(VecOwn<ram::Statement>& curStmts, const ast::Relation* relation) {
    for (auto directives : getOutputDirectives(relation)) {
        Own<ram::Statement> statement = mk<ram::IO>(getConcreteRelationName(relation), directives);
        if (Global::config().has("profile")) {
            const std::string logTimerStatement = LogStatement::tRelationSaveTime(
                    toString(relation->getQualifiedName()), relation->getSrcLoc());
            statement = mk<ram::LogRelationTimer>(
                    std::move(statement), logTimerStatement, getConcreteRelationName(relation));
        }
        appendStmt(curStmts, std::move(statement));
    }
}

void AstToRamTranslator::createRamRelation(size_t scc) {
    const auto& isRecursive = sccGraph->isRecursive(scc);
    const auto& sccRelations = sccGraph->getInternalRelations(scc);
    for (const auto& rel : sccRelations) {
        std::string name = getRelationName(rel->getQualifiedName());
        auto arity = rel->getArity();
        auto auxiliaryArity = auxArityAnalysis->getArity(rel);
        auto representation = rel->getRepresentation();
        const auto& attributes = rel->getAttributes();

        std::vector<std::string> attributeNames;
        std::vector<std::string> attributeTypeQualifiers;
        for (size_t i = 0; i < rel->getArity(); ++i) {
            attributeNames.push_back(attributes[i]->getName());
            if (typeEnv != nullptr) {
                attributeTypeQualifiers.push_back(
                        getTypeQualifier(typeEnv->getType(attributes[i]->getTypeName())));
            }
        }
        auto ramRelation = mk<ram::Relation>(
                name, arity, auxiliaryArity, attributeNames, attributeTypeQualifiers, representation);
        addRamRelation(name, std::move(ramRelation));

        // recursive relations also require @delta and @new variants, with the same signature
        if (isRecursive) {
            // add delta relation
            std::string deltaName = getDeltaRelationName(rel);
            auto deltaRelation = mk<ram::Relation>(deltaName, arity, auxiliaryArity, attributeNames,
                    attributeTypeQualifiers, representation);
            addRamRelation(deltaName, std::move(deltaRelation));

            // add new relation
            std::string newName = getNewRelationName(rel);
            auto newRelation = mk<ram::Relation>(
                    newName, arity, auxiliaryArity, attributeNames, attributeTypeQualifiers, representation);
            addRamRelation(newName, std::move(newRelation));
        }
    }
}

const ram::Relation* AstToRamTranslator::lookupRelation(const std::string& name) const {
    assert(contains(ramRelations, name) && "relation not found");
    return ramRelations.at(name).get();
}

void AstToRamTranslator::finaliseAstTypes() {
    visitDepthFirst(*program, [&](const ast::NumericConstant& nc) {
        const_cast<ast::NumericConstant&>(nc).setFinalType(polyAnalysis->getInferredType(&nc));
    });
    visitDepthFirst(*program, [&](const ast::Aggregator& aggr) {
        const_cast<ast::Aggregator&>(aggr).setFinalType(polyAnalysis->getOverloadedOperator(&aggr));
    });
    visitDepthFirst(*program, [&](const ast::BinaryConstraint& bc) {
        const_cast<ast::BinaryConstraint&>(bc).setFinalType(polyAnalysis->getOverloadedOperator(&bc));
    });
    visitDepthFirst(*program, [&](const ast::IntrinsicFunctor& inf) {
        const_cast<ast::IntrinsicFunctor&>(inf).setFinalOpType(polyAnalysis->getOverloadedFunctionOp(&inf));
        const_cast<ast::IntrinsicFunctor&>(inf).setFinalReturnType(functorAnalysis->getReturnType(&inf));
    });
    visitDepthFirst(*program, [&](const ast::UserDefinedFunctor& udf) {
        const_cast<ast::UserDefinedFunctor&>(udf).setFinalReturnType(functorAnalysis->getReturnType(&udf));
    });
}

Own<ram::Sequence> AstToRamTranslator::translateProgram(const ast::TranslationUnit& translationUnit) {
    // keep track of relevant analyses
    ioType = translationUnit.getAnalysis<ast::analysis::IOTypeAnalysis>();
    typeEnv = &translationUnit.getAnalysis<ast::analysis::TypeEnvironmentAnalysis>()->getTypeEnvironment();
    relationSchedule = translationUnit.getAnalysis<ast::analysis::RelationScheduleAnalysis>();
    sccGraph = translationUnit.getAnalysis<ast::analysis::SCCGraphAnalysis>();
    recursiveClauses = translationUnit.getAnalysis<ast::analysis::RecursiveClausesAnalysis>();
    auxArityAnalysis = translationUnit.getAnalysis<ast::analysis::AuxiliaryArityAnalysis>();
    functorAnalysis = translationUnit.getAnalysis<ast::analysis::FunctorAnalysis>();
    relDetail = translationUnit.getAnalysis<ast::analysis::RelationDetailCacheAnalysis>();
    polyAnalysis = translationUnit.getAnalysis<ast::analysis::PolymorphicObjectsAnalysis>();

    // finalise polymorphic types in the AST
    finaliseAstTypes();

    // determine the sips to use
    std::string sipsChosen = "all-bound";
    if (Global::config().has("RamSIPS")) {
        sipsChosen = Global::config().get("RamSIPS");
    }
    sipsMetric = ast::SipsMetric::create(sipsChosen, translationUnit);

    // replace ADTs with record representatives
    removeADTs(translationUnit);

    // handle the case of an empty SCC graph
    if (sccGraph->getNumberOfSCCs() == 0) return mk<ram::Sequence>();

    // create all RAM relations
    const auto& sccOrdering =
            translationUnit.getAnalysis<ast::analysis::TopologicallySortedSCCGraphAnalysis>()->order();
    for (const auto& scc : sccOrdering) {
        createRamRelation(scc);
    }

    // create subroutine for each SCC according to topological order
    for (size_t i = 0; i < sccOrdering.size(); i++) {
        auto sccCode = translateSCC(sccOrdering.at(i), i);
        std::string stratumID = "stratum_" + toString(i);
        addRamSubroutine(stratumID, std::move(sccCode));
    }

    // invoke all strata
    VecOwn<ram::Statement> res;
    for (size_t i = 0; i < sccOrdering.size(); i++) {
        appendStmt(res, mk<ram::Call>("stratum_" + toString(i)));
    }

    // add main timer if profiling
    if (res.size() > 0 && Global::config().has("profile")) {
        auto newStmt = mk<ram::LogTimer>(mk<ram::Sequence>(std::move(res)), LogStatement::runtime());
        res.clear();
        appendStmt(res, std::move(newStmt));
    }

    // done for main prog
    return mk<ram::Sequence>(std::move(res));
}

Own<ram::TranslationUnit> AstToRamTranslator::translateUnit(ast::TranslationUnit& tu) {
    auto ram_start = std::chrono::high_resolution_clock::now();
    program = &tu.getProgram();
    symbolTable = mk<SymbolTable>();

    auto ramMain = translateProgram(tu);

    ErrorReport& errReport = tu.getErrorReport();
    DebugReport& debugReport = tu.getDebugReport();
    VecOwn<ram::Relation> rels;
    for (auto& cur : ramRelations) {
        rels.push_back(std::move(cur.second));
    }

    auto ramProg = mk<ram::Program>(std::move(rels), std::move(ramMain), std::move(ramSubroutines));

    // add the translated program to the debug report
    if (Global::config().has("debug-report")) {
        auto ram_end = std::chrono::high_resolution_clock::now();
        std::string runtimeStr =
                "(" + std::to_string(std::chrono::duration<double>(ram_end - ram_start).count()) + "s)";
        std::stringstream ramProgStr;
        ramProgStr << *ramProg;
        debugReport.addSection("ram-program", "RAM Program " + runtimeStr, ramProgStr.str());
    }

    return mk<ram::TranslationUnit>(std::move(ramProg), *symbolTable, errReport, debugReport);
}

}  // namespace souffle::ast2ram
