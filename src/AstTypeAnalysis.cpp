/*
 * Souffle - A Datalog Compiler
 * Copyright (c) 2019, The Souffle Developers. All rights reserved.
 * Licensed under the Universal Permissive License v 1.0 as shown at:
 * - https://opensource.org/licenses/UPL
 * - <souffle root>/licenses/SOUFFLE-UPL.txt
 */

/************************************************************************
 *
 * @file AstTypeAnalysis.cpp
 *
 * A type analysis class operating on AST programs.
 *
 ***********************************************************************/

#include "AstTypeAnalysis.h"
#include "AstTranslationUnit.h"
#include "AstTypeEnvironmentAnalysis.h"
#include "Global.h"
#include "TypeConstraint.h"
#include <ostream>

namespace souffle {

void TypeSolver::generateConstraints() {
    // Helper class to find all constraints imposed by a clause
    class ConstraintFinder : public AstVisitor<void> {
    public:
        ConstraintFinder(const AstProgram* program, const TypeLattice* lattice, TypeSolver* solver)
                : program(program), lattice(lattice), solver(solver) {}

        void visitNode(const AstNode& node) {
            // by default, extract the constraints generated by all children
            for (const AstNode* child : node.getChildNodes()) {
                visit(*child);
            }
        }

        void visitCounter(const AstCounter& counter) {
            // counters must be numbers
            solver->addConstraint(std::make_unique<FixedConstraint>(
                    &counter, std::make_unique<ConstantAnalysisType>(Kind::NUMBER)));
        }

        void visitNumberConstant(const AstNumberConstant& constant) {
            // number constants must actually be numbers
            solver->addConstraint(std::make_unique<FixedConstraint>(
                    &constant, std::make_unique<ConstantAnalysisType>(Kind::NUMBER)));
        }

        void visitStringConstant(const AstStringConstant& constant) {
            // string constants must actually be strings
            solver->addConstraint(std::make_unique<FixedConstraint>(
                    &constant, std::make_unique<ConstantAnalysisType>(Kind::SYMBOL)));
        }

        void visitNullConstant(const AstNullConstant& constant) {
            // nils must be record types
            solver->addConstraint(std::make_unique<FixedConstraint>(
                    &constant, std::make_unique<ConstantAnalysisType>(Kind::RECORD)));
        }

        void visitTypeCast(const AstTypeCast& cast) {
            // extract child constraints
            visitNode(cast);

            // argument must be of the resultant type
            const AnalysisType* type = lattice->getAnalysisType(cast.getType());
            solver->addConstraint(
                    std::make_unique<FixedConstraint>(&cast, std::unique_ptr<AnalysisType>(type->clone())));
        }

        void visitIntrinsicFunctor(const AstIntrinsicFunctor& functor) {
            // extract child constraints
            visitNode(functor);

            // get the constraints forced by the functor itself
            if (functor.getFunction() == FunctorOp::MAX || functor.getFunction() == FunctorOp::MIN) {
                // result of max and min must be one of the types
                const AstArgument* lhs = solver->getRepresentative(functor.getArg(0));
                const AstArgument* rhs = solver->getRepresentative(functor.getArg(1));
                solver->addConstraint(std::make_unique<UnionConstraint>(&functor, lhs, rhs));
            } else {
                // grab the kind of the functor
                Kind kind;
                if (functor.isSymbolic()) {
                    kind = Kind::SYMBOL;
                } else if (functor.isNumerical()) {
                    kind = Kind::NUMBER;
                } else {
                    assert(false && "unsupported functor output type");
                }

                // restrict the output type of the functor
                solver->addConstraint(std::make_unique<FixedConstraint>(
                        &functor, std::make_unique<TopPrimitiveAnalysisType>(kind)));

                // functor applied to constants must give a constant
                auto constantConstraint =
                        std::make_unique<ImplicationConstraint>(std::make_unique<FixedConstraint>(
                                &functor, std::make_unique<ConstantAnalysisType>(kind)));
                for (size_t i = 0; i < functor.getArity(); i++) {
                    const AstArgument* arg = solver->getRepresentative(functor.getArg(i));
                    if (functor.acceptsSymbols(i)) {
                        constantConstraint->addRequirement(std::make_unique<FixedConstraint>(
                                arg, std::make_unique<ConstantAnalysisType>(Kind::SYMBOL)));
                    } else if (functor.acceptsNumbers(i)) {
                        constantConstraint->addRequirement(std::make_unique<FixedConstraint>(
                                arg, std::make_unique<ConstantAnalysisType>(Kind::NUMBER)));
                    } else {
                        assert(false && "unsupported functor argument type");
                    }
                }
                solver->addConstraint(std::move(constantConstraint));
            }
        }

        void visitUserDefinedFunctor(const AstUserDefinedFunctor& functor) {
            // extract child constraints
            visitNode(functor);

            // -- get the constraints forced by the functor itself --

            // grab the kind of the functor
            AstFunctorDeclaration* funDecl = program->getFunctorDeclaration(functor.getName());
            Kind kind;
            if (funDecl->isSymbolic()) {
                kind = Kind::SYMBOL;
            } else if (funDecl->isNumerical()) {
                kind = Kind::NUMBER;
            } else {
                assert(false && "unsupported functor out put type");
            }

            // restrict the output type of the functor
            solver->addConstraint(std::make_unique<FixedConstraint>(
                    &functor, std::make_unique<TopPrimitiveAnalysisType>(kind)));

            // functor applied to constants must give a constant
            auto constantConstraint =
                    std::make_unique<ImplicationConstraint>(std::make_unique<FixedConstraint>(
                            &functor, std::make_unique<ConstantAnalysisType>(kind)));
            for (size_t i = 0; i < functor.getArgCount(); i++) {
                const AstArgument* arg = solver->getRepresentative(functor.getArg(i));
                if (funDecl->acceptsSymbols(i)) {
                    constantConstraint->addRequirement(std::make_unique<FixedConstraint>(
                            arg, std::make_unique<ConstantAnalysisType>(Kind::SYMBOL)));
                } else if (funDecl->acceptsNumbers(i)) {
                    constantConstraint->addRequirement(std::make_unique<FixedConstraint>(
                            arg, std::make_unique<ConstantAnalysisType>(Kind::NUMBER)));
                } else {
                    assert(false && "unsupported functor argument type");
                }
            }
            solver->addConstraint(std::move(constantConstraint));
        }

        void visitRecordInit(const AstRecordInit& record) {
            // extract child constraints
            visitNode(record);

            // two scenarios must be considered:
            // (1) the type of the record has been bound to any record type:
            //      - the record is therefore directly grounded
            //      - all arguments must be bound to their expected type
            // (2) all arguments have been bound to their expected type
            //      - the record is therefore grounded via its arguments
            //      - the record must be bound to its expected type
            // the semantic checker
            const auto* typeEnv = lattice->getTypeEnvironment();
            const auto* rawType = dynamic_cast<const RecordType*>(&typeEnv->getType(record.getType()));
            assert(rawType != nullptr && "type of record must be a record type");
            assert(record.getArguments().size() == rawType->getFields().size() &&
                    "record constructor has incorrect number of arguments");

            const auto* recordType = lattice->getAnalysisType(*rawType);
            const auto& fields = rawType->getFields();
            const auto& args = record.getArguments();

            // cover (1)
            for (size_t i = 0; i < args.size(); i++) {
                const AstArgument* arg = solver->getRepresentative(args[i]);
                const AnalysisType* fieldType = lattice->getAnalysisType(fields[i].type);

                // construct the implication constraint
                auto requirement = std::make_unique<FixedConstraint>(
                        &record, std::make_unique<TopPrimitiveAnalysisType>(Kind::RECORD));
                auto consequent = std::make_unique<FixedConstraint>(
                        arg, std::unique_ptr<AnalysisType>(fieldType->clone()));

                auto newConstraint = std::make_unique<ImplicationConstraint>(std::move(consequent));
                newConstraint->addRequirement(std::move(requirement));

                // add it in
                solver->addConstraint(std::move(newConstraint));
            }

            // cover (2)
            auto consequent = std::make_unique<FixedConstraint>(
                    &record, std::unique_ptr<AnalysisType>(recordType->clone()));
            auto finalConstraint = std::make_unique<ImplicationConstraint>(std::move(consequent));

            for (size_t i = 0; i < args.size(); i++) {
                const AstArgument* arg = solver->getRepresentative(args[i]);
                const AnalysisType* fieldType = lattice->getAnalysisType(fields[i].type);

                // add the new requirement
                auto newRequirement = std::make_unique<FixedConstraint>(
                        arg, std::unique_ptr<AnalysisType>(fieldType->clone()));
                finalConstraint->addRequirement(std::move(newRequirement));
            }

            // add in the constraint
            solver->addConstraint(std::move(finalConstraint));
        }

        void visitAggregator(const AstAggregator& aggregate) {
            // extract child constraints
            visitNode(aggregate);

            auto op = aggregate.getOperator();
            if (op == AstAggregator::count || op == AstAggregator::sum) {
                // aggregator type is just a number
                auto newConstraint = std::make_unique<FixedConstraint>(
                        &aggregate, std::make_unique<TopPrimitiveAnalysisType>(Kind::NUMBER));
                solver->addConstraint(std::move(newConstraint));
            } else if (op == AstAggregator::min || op == AstAggregator::max) {
                // aggregator type must match target expression
                const AstArgument* targetExpression =
                        solver->getRepresentative(aggregate.getTargetExpression());
                auto newConstraint = std::make_unique<VariableConstraint>(&aggregate, targetExpression);
                solver->addConstraint(std::move(newConstraint));
            } else {
                assert(false && "unsupported aggregation operator");
            }
        }

        void visitAtom(const AstAtom& atom) {
            // extract child constraints
            visitNode(atom);

            // atom arguments must have the correct type
            AstRelation* rel = program->getRelation(atom.getName());
            assert(rel->getArity() == atom.getArity() && "atom has incorrect number of arguments");
            for (size_t i = 0; i < atom.getArity(); i++) {
                const AstArgument* arg = solver->getRepresentative(atom.getArgument(i));
                const AnalysisType* expectedType =
                        lattice->getAnalysisType(rel->getAttribute(i)->getTypeName());
                solver->addConstraint(std::make_unique<FixedConstraint>(
                        arg, std::unique_ptr<AnalysisType>(expectedType->clone())));
            }
        }

        void visitNegation(const AstNegation& negation) {
            // only extract child constraints of the internal atom
            return visitNode(*negation.getAtom());
        }

        void visitBinaryConstraint(const AstBinaryConstraint& binary) {
            // extract child constraints
            visitNode(binary);

            // equality implies equivalent types
            if (binary.getOperator() == BinaryConstraintOp::EQ) {
                const AstArgument* lhs = solver->getRepresentative(binary.getLHS());
                const AstArgument* rhs = solver->getRepresentative(binary.getRHS());
                solver->addConstraint(std::make_unique<VariableConstraint>(lhs, rhs));
                solver->addConstraint(std::make_unique<VariableConstraint>(rhs, lhs));
            }
        }

        void visitClause(const AstClause& clause) {
            // get constraints from body literals only
            for (const AstLiteral* literal : clause.getBodyLiterals()) {
                visit(*literal);
            }

            // get constraints generated by the children of the head
            // the head itself should be ignored
            visitNode(*clause.getHead());
        }

    private:
        const AstProgram* program;
        const TypeLattice* lattice;
        TypeSolver* solver;
    };

    ConstraintFinder(program, lattice, this).visit(*clause);
}

void TypeSolver::resolveConstraints() {
    // restore everything to the top type
    typeMapping.clear();
    visitDepthFirst(*clause,
            [&](const AstArgument& arg) { setType(&arg, lattice->getStoredType(TopAnalysisType())); });

    // apply each constraint until all are satisfied (fixed point reached)
    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto& cons : constraints) {
            if (!cons->isSatisfied(this)) {
                changed = true;
                cons->resolve(this);
            }
        }
    }

    if (logStream != nullptr) {
        *logStream << "Clause:\n" << *clause << std::endl << std::endl;
        *logStream << "\tConstraints:" << std::endl;
        for (const auto& cons : constraints) {
            *logStream << "\t\t" << *cons << std::endl;
        }
        *logStream << "\tTypes:\n" << std::endl;
        for (const auto& pair : typeMapping) {
            *logStream << "\t\t"
                       << "type(" << *pair.first << ") = " << *pair.second << std::endl;
        }
        *logStream << std::endl;
    }
}

const AstArgument* TypeSolver::getRepresentative(const AstArgument* arg) const {
    // non-variables are not affected
    if (dynamic_cast<const AstVariable*>(arg) == nullptr) {
        return arg;
    }

    const auto* var = dynamic_cast<const AstVariable*>(arg);
    assert(var != nullptr && "expected variable type");

    // check if already represented
    const std::string name = var->getName();
    if (representatives.find(name) != representatives.end()) {
        return representatives[name];
    }

    // otherwise, set it as the representative
    representatives[name] = var;
    return var;
}

void TypeAnalysis::run(const AstTranslationUnit& translationUnit) {
    // set where debug information should be sent
    // TODO: why sometimes ostream sometimes stringstream
    std::stringstream* debugStream = nullptr;
    if (!Global::config().get("debug-report").empty()) {
        debugStream = &logStream;
    }

    // clear up existing data
    typeSolutions.clear();
    typedClauses.clear();
    hasInvalidClauses = false;
    logStream.str("");
    logStream.clear();

    // set up a new type lattice
    auto* typeEnvAnalysis = translationUnit.getAnalysis<TypeEnvironmentAnalysis>();
    lattice = std::make_unique<TypeLattice>(&typeEnvAnalysis->getTypeEnvironment());

    // run a type analysis on each clause
    if (lattice->isValid()) {
        const AstProgram* program = translationUnit.getProgram();

        for (const AstRelation* rel : program->getRelations()) {
            for (const AstClause* clause : rel->getClauses()) {
                // check if it can be typechecked
                if (!TypeAnalysis::isInvalidClause(program, clause)) {
                    hasInvalidClauses = true;
                    continue;
                }
                typedClauses.push_back(clause);

                // perform the type analysis
                TypeSolver solver(program, lattice.get(), clause, debugStream);

                // store the result for each argument
                visitDepthFirst(*clause, [&](const AstArgument& arg) {
                    assert(solver.hasType(&arg) && "clause argument does not have a type");
                    typeSolutions[&arg] = solver.getType(&arg);
                });
            }
        }

        if (debugStream != nullptr && hasInvalidClauses) {
            *debugStream << std::endl
                         << "Some clauses were skipped as they cannot be typechecked" << std::endl;
        }
    }
}

bool TypeAnalysis::isInvalidClause(const AstProgram* program, const AstClause* clause) {
    bool valid = true;

    // -- check atoms --
    visitDepthFirst(*clause, [&](const AstAtom& atom) {
        auto* rel = program->getRelation(atom.getName());
        if (rel == nullptr) {
            // undefined relation
            valid = false;
        } else if (rel->getArity() != atom.getArity()) {
            // non-matching arity
            valid = false;
        } else {
            // all attributes should have defined types
            for (const auto* attr : rel->getAttributes()) {
                const auto& typeName = attr->getTypeName();
                if (typeName == "symbol" || typeName == "number") {
                    // primitive type - valid
                    continue;
                }

                if (program->getType(typeName) == nullptr) {
                    // undefined type
                    valid = false;
                    break;
                }
            }
        }
    });

    // -- check user-defined functors --
    visitDepthFirst(*clause, [&](const AstUserDefinedFunctor& fun) {
        AstFunctorDeclaration* funDecl = program->getFunctorDeclaration(fun.getName());
        if (funDecl == nullptr) {
            // undefined functor
            valid = false;
        } else if (funDecl->getArgCount() != fun.getArgCount()) {
            // non-matching arity
            valid = false;
        }
    });

    // -- check records --
    visitDepthFirst(*clause, [&](const AstRecordInit& record) {
        const auto* recordType = dynamic_cast<const AstRecordType*>(program->getType(record.getType()));
        if (recordType == nullptr) {
            // record should have a record type
            valid = false;
        } else if (record.getArguments().size() != recordType->getFields().size()) {
            // invalid record arity
            valid = false;
        }
    });

    // -- check typecasts --
    visitDepthFirst(*clause, [&](const AstTypeCast& cast) {
        const auto& typeName = cast.getType();
        if (typeName == "symbol" || typeName == "number") {
            // primitive type - valid
            return;
        }

        if (program->getType(typeName) == nullptr) {
            // undefined type
            valid = false;
        }
    });

    return valid;
}

}  // end of namespace souffle
