/***************************************************************************
 *   Copyright (c) Eivind Kvedalen (eivind@kvedalen.name) 2015             *
 *                                                                         *
 *   This file is part of the FreeCAD CAx development system.              *
 *                                                                         *
 *   This library is free software; you can redistribute it and/or         *
 *   modify it under the terms of the GNU Library General Public           *
 *   License as published by the Free Software Foundation; either          *
 *   version 2 of the License, or (at your option) any later version.      *
 *                                                                         *
 *   This library  is distributed in the hope that it will be useful,      *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU Library General Public License for more details.                  *
 *                                                                         *
 *   You should have received a copy of the GNU Library General Public     *
 *   License along with this library; see the file COPYING.LIB. If not,    *
 *   write to the Free Software Foundation, Inc., 59 Temple Place,         *
 *   Suite 330, Boston, MA  02111-1307, USA                                *
 *                                                                         *
 ***************************************************************************/

#include "PreCompiled.h"
#include <App/Application.h>
#include <App/Document.h>
#include <App/DocumentObject.h>
#include <Base/Interpreter.h>
#include <Base/Writer.h>
#include <Base/Reader.h>
#include "Expression.h"
#include "ExpressionVisitors.h"
#include "PropertyExpressionEngine.h"
#include "PropertyStandard.h"
#include "PropertyUnits.h"
#include <CXX/Objects.hxx>
#include <boost/bind.hpp>
#include <boost/graph/graph_traits.hpp>
#include <boost/graph/adjacency_list.hpp>


using namespace App;
using namespace Base;
using namespace boost;

static inline std::string objName(App::DocumentObject *obj) {
    if(obj && obj->getNameInDocument())
        return std::string(obj->getDocument()->getName()) + '#' + obj->getNameInDocument();
    return "?";
}

class ObjectDeletedExpressionVisitor : public ExpressionVisitor {
public:

    ObjectDeletedExpressionVisitor(const App::DocumentObject * _obj)
        : obj(_obj)
        , found(false)
    {
    }

    /**
     * @brief Visit each node in the expression, and if it is a VariableExpression object check if it references obj
     * @param node Node to visit
     */

    void visit(Expression * node) {
        VariableExpression *expr = freecad_dynamic_cast<VariableExpression>(node);

        if (expr && expr->getPath().getDocumentObject() == obj)
            found = true;
    }

    bool isFound() const { return found; }

private:
    const App::DocumentObject * obj;
    bool found;
};

TYPESYSTEM_SOURCE(App::PropertyExpressionEngine , App::Property);

/**
 * @brief Construct a new PropertyExpressionEngine object.
 */

PropertyExpressionEngine::PropertyExpressionEngine()
    : Property()
    , AtomicPropertyChangeInterface()
    , running(false)
    , validator(0)
{
}

/**
 * @brief Destroy the PropertyExpressionEngine object.
 */

PropertyExpressionEngine::~PropertyExpressionEngine()
{
}

/**
 * @brief Estimate memory size of this property.
 *
 * \fixme Should probably return something else than 0.
 *
 * @return Size of object.
 */

unsigned int PropertyExpressionEngine::getMemSize() const
{
    return 0;
}

Property *PropertyExpressionEngine::Copy() const
{
    PropertyExpressionEngine * engine = new PropertyExpressionEngine();

    for (ExpressionMap::const_iterator it = expressions.begin(); it != expressions.end(); ++it)
        engine->expressions[it->first] = ExpressionInfo(boost::shared_ptr<Expression>(it->second.expression->copy()), it->second.comment.c_str());

    engine->validator = validator;

    return engine;
}

void PropertyExpressionEngine::Paste(const Property &from)
{
    const PropertyExpressionEngine * fromee = static_cast<const PropertyExpressionEngine*>(&from);

    AtomicPropertyChange signaller(*this);

#ifndef USE_OLD_DAG
    //maintain backlinks, verify that this property is owned by a DocumentObject
    App::DocumentObject* parent = dynamic_cast<App::DocumentObject*>(getContainer());
    if (parent) {
        for(auto &v : expressions) {
            for(auto docObj : v.second.expression->getDepObjects()) {
                if (docObj && (docObj != parent))
                    docObj->_removeBackLink(parent);
            }
        }
    }
#endif
    expressions.clear();

    for (ExpressionMap::const_iterator it = fromee->expressions.begin(); it != fromee->expressions.end(); ++it) {
        expressions[it->first] = ExpressionInfo(boost::shared_ptr<Expression>(it->second.expression->copy()), it->second.comment.c_str());

#ifndef USE_OLD_DAG
        if (parent) {
            //maintain backlinks
            for(auto docObj : it->second.expression->getDepObjects()) {
                if (docObj && (docObj != parent))
                    docObj->_addBackLink(parent);
            }
        }
#endif

        expressionChanged(it->first);
    }

    validator = fromee->validator;
}

void PropertyExpressionEngine::Save(Base::Writer &writer) const
{
    writer.Stream() << writer.ind() << "<ExpressionEngine count=\"" <<  expressions.size() <<"\">" << std::endl;
    writer.incInd();
    for (ExpressionMap::const_iterator it = expressions.begin(); it != expressions.end(); ++it) {
        writer.Stream() << writer.ind() << "<Expression path=\"" <<  Property::encodeAttribute(it->first.toString()) <<"\"" <<
                           " expression=\"" << Property::encodeAttribute(it->second.expression->toString()) << "\"";
        if (it->second.comment.size() > 0)
            writer.Stream() << " comment=\"" << Property::encodeAttribute(it->second.comment) << "\"";
        writer.Stream() << "/>" << std::endl;
    }
    writer.decInd();
    writer.Stream() << writer.ind() << "</ExpressionEngine>" << std::endl;
}

void PropertyExpressionEngine::Restore(Base::XMLReader &reader)
{
    reader.readElement("ExpressionEngine");

    int count = reader.getAttributeAsFloat("count");

    restoredExpressions.clear();
    for (int i = 0; i < count; ++i) {
        DocumentObject * docObj = freecad_dynamic_cast<DocumentObject>(getContainer());

        reader.readElement("Expression");
        ObjectIdentifier path = ObjectIdentifier::parse(docObj, reader.getAttribute("path"));
        boost::shared_ptr<Expression> expression(ExpressionParser::parse(docObj, reader.getAttribute("expression")));
        const char * comment = reader.hasAttribute("comment") ? reader.getAttribute("comment") : 0;

        restoredExpressions[path] = ExpressionInfo(expression, comment);
    }

    reader.readEndElement("ExpressionEngine");
}

/**
 * @brief Update graph structure with given path and expression.
 * @param path Path
 * @param expression Expression to query for dependencies
 * @param nodes Map with nodes of graph
 * @param revNodes Reverse map of nodes
 * @param edges Edges in graph
 */

void PropertyExpressionEngine::buildGraphStructures(const ObjectIdentifier & path,
                                                    const boost::shared_ptr<Expression> expression,
                                                    boost::unordered_map<ObjectIdentifier, int> & nodes,
                                                    boost::unordered_map<int, ObjectIdentifier> & revNodes,
                                                    std::vector<Edge> & edges) const
{
    /* Insert target property into nodes structure */
    if (nodes.find(path) == nodes.end()) {
        int s = nodes.size();

        revNodes[s] = path;
        nodes[path] = s;
    }
    else
        revNodes[nodes[path]] = path;

    /* Insert dependencies into nodes structure */
    for(auto &dep : expression->getDeps()) {
        for(auto &info : dep.second) {
            if(info.first.empty())
                continue;
            for(auto &oid : info.second) {
                ObjectIdentifier cPath(oid.canonicalPath());
                if (nodes.find(cPath) == nodes.end()) {
                    int s = nodes.size();
                    nodes[cPath] = s;
                }
                edges.push_back(std::make_pair(nodes[path], nodes[cPath]));
            }
        }
    }
}

/**
 * @brief Create a canonical object identifier of the given object \a p.
 * @param p ObjectIndentifier
 * @return New ObjectIdentifier
 */

const ObjectIdentifier PropertyExpressionEngine::canonicalPath(const ObjectIdentifier &p) const
{
    DocumentObject * docObj = freecad_dynamic_cast<DocumentObject>(getContainer());

    // Am I owned by a DocumentObject?
    if (!docObj)
        throw Base::RuntimeError("PropertyExpressionEngine must be owned by a DocumentObject.");

    int ptype;
    Property * prop = p.getProperty(&ptype);

    // p pointing to a property...?
    if (!prop)
        throw Base::RuntimeError(p.resolveErrorString().c_str());

    if(ptype || prop->getContainer()!=getContainer())
        return p;

    // In case someone calls this with p pointing to a PropertyExpressionEngine for some reason
    if (prop->isDerivedFrom(PropertyExpressionEngine::classTypeId))
        return p;

    // Dispatch call to actual canonicalPath implementation
    return p.canonicalPath();
}

/**
 * @brief Number of expressions managed by this object.
 * @return Number of expressions.
 */

size_t PropertyExpressionEngine::numExpressions() const
{
    return expressions.size();
}

/**
 * @brief Slot called when a document object is renamed.
 * @param obj Renamed object
 */

void PropertyExpressionEngine::slotObjectRenamed(const DocumentObject &obj)
{
#ifdef FC_PROPERTYEXPRESSIONENGINE_LOG
    std::clog << "Object " << obj.getOldLabel() << " renamed to " << obj.Label.getValue() << std::endl;
#endif

    DocumentObject * docObj = freecad_dynamic_cast<DocumentObject>(getContainer());

    /* In a document object, and on undo stack? */
    if (!docObj || docObj->getNameInDocument() == 0)
        return;

    RelabelDocumentObjectExpressionVisitor<PropertyExpressionEngine> v(*this, &obj);

    for (ExpressionMap::iterator it = expressions.begin(); it != expressions.end(); ++it) {
        bool changed = v.getChanged();

        it->second.expression->visit(v);

        if (changed != v.getChanged())
            expressionChanged(it->first);
    }
}

void PropertyExpressionEngine::slotObjectDeleted(const DocumentObject &obj)
{
    DocumentObject * docObj = freecad_dynamic_cast<DocumentObject>(getContainer());

    /* In a document object, and on undo stack? */
    if (!docObj || docObj->getNameInDocument() == 0)
        return;

    ObjectDeletedExpressionVisitor v(&obj);

    for (ExpressionMap::iterator it = expressions.begin(); it != expressions.end(); ++it) {
        it->second.expression->visit(v);

        if (v.isFound()) {
            touch(); // Touch to force recompute; that will trigger a proper error
            return;
        }
    }
}

void PropertyExpressionEngine::onDocumentRestored()
{
    AtomicPropertyChange signaller(*this);

    for (ExpressionMap::iterator it = restoredExpressions.begin(); it != restoredExpressions.end(); ++it)
        setValue(it->first, it->second.expression, it->second.comment.size() > 0 ? it->second.comment.c_str() : 0);
}

/**
 * @brief Get expression for \a path.
 * @param path ObjectIndentifier to query for.
 * @return Expression for \a path, or empty boost::any if not found.
 */

const boost::any PropertyExpressionEngine::getPathValue(const App::ObjectIdentifier & path) const
{
    // Get a canonical path
    ObjectIdentifier usePath(canonicalPath(path));

    ExpressionMap::const_iterator i = expressions.find(usePath);

    if (i != expressions.end())
        return i->second;
    else
        return boost::any();
}

/**
 * @brief Set expression with optional comment for \a path.
 * @param path Path to update
 * @param expr New expression
 * @param comment Optional comment.
 */

void PropertyExpressionEngine::setValue(const ObjectIdentifier & path, boost::shared_ptr<Expression> expr, const char *comment)
{
    ObjectIdentifier usePath(canonicalPath(path));
    const Property * prop = usePath.getProperty();

    // Try to access value; it should trigger an exception if it is not supported, or if the path is invalid
    prop->getPathValue(usePath);

    // Check if the current expression equals the new one and do nothing if so to reduce unneeded computations
    ExpressionMap::iterator it = expressions.find(usePath);
    if(it != expressions.end() && expr == it->second.expression)
        return;
    
    if (expr) {
        std::string error = validateExpression(usePath, expr);

        if (error.size() > 0)
            throw Base::RuntimeError(error.c_str());

        AtomicPropertyChange signaller(*this);
#ifndef USE_OLD_DAG
        // When overriding an ObjectIdentifier key then first remove
        // the dependency caused by the expression as otherwise it happens
        // that the same object dependency is added twice for the same
        // identifier. This makes it impossible to properly clear dependencies
        // and thus leads to topological errors on recompute.
        //
        // Verify that this property is owned by a DocumentObject
        App::DocumentObject* parent = dynamic_cast<App::DocumentObject*>(getContainer());
        if (parent) {
            if (it != expressions.end() && it->second.expression) {
                for(auto docObj : it->second.expression->getDepObjects()) {
                    if (docObj && (docObj != parent))
                        docObj->_removeBackLink(parent);
                }
            }
        }
#endif

        expressions[usePath] = ExpressionInfo(expr, comment);

#ifndef USE_OLD_DAG
        //maintain the backlinks in the documentobject graph datastructure
        if (parent) {
            for(auto docObj : expr->getDepObjects()) {
                if (docObj && (docObj != parent))
                    docObj->_addBackLink(parent);
            }
        }
#endif

        expressionChanged(usePath);
    }
    else {
        AtomicPropertyChange signaller(*this);

#ifndef USE_OLD_DAG
        //verify that this property is owned by a DocumentObject
        //verify that the ObjectIdentifier usePath is part of the expression map and
        //that the expression is not null
        App::DocumentObject* parent = dynamic_cast<App::DocumentObject*>(getContainer());
        if (parent && it != expressions.end() && it->second.expression) {
            //maintain the backlinks in the documentobject graph datastructure
            for(auto docObj : it->second.expression->getDepObjects()) {
                if (docObj && (docObj != parent))
                    docObj->_removeBackLink(parent);
            }
        }
#endif

        expressions.erase(usePath);
        expressionChanged(usePath);
    }
}

/**
 * @brief The cycle_detector struct is used by the boost graph routines to detect cycles in the graph.
 */

struct cycle_detector : public boost::dfs_visitor<> {
    cycle_detector( bool& has_cycle, int & src)
      : _has_cycle(has_cycle), _src(src) { }

    template <class Edge, class Graph>
    void back_edge(Edge e, Graph&g) {
      _has_cycle = true;
      _src = source(e, g);
    }

  protected:
    bool& _has_cycle;
    int & _src;
};

/**
 * @brief Build a graph of all expressions in \a exprs.
 * @param exprs Expressions to use in graph
 * @param revNodes Map from int to ObjectIndentifer
 * @param g Graph to update
 */

void PropertyExpressionEngine::buildGraph(const ExpressionMap & exprs,
                    boost::unordered_map<int, ObjectIdentifier> & revNodes, DiGraph & g, int output) const
{
    boost::unordered_map<ObjectIdentifier, int> nodes;
    std::vector<Edge> edges;

    // Build data structure for graph
    for (ExpressionMap::const_iterator it = exprs.begin(); it != exprs.end(); ++it) {
        if(output>=0) {
            auto prop = it->first.getProperty();
            if(!prop)
                throw Base::RuntimeError("Path does not resolve to a property.");
            bool is_output = prop->testStatus(App::Property::Output)||(prop->getType()&App::Prop_Output);
            if(is_output != (output>0))
                continue;
        }
        buildGraphStructures(it->first, it->second.expression, nodes, revNodes, edges);
    }

    // Create graph
    g = DiGraph(revNodes.size());

    // Add edges to graph
    for (std::vector<Edge>::const_iterator i = edges.begin(); i != edges.end(); ++i)
        add_edge(i->first, i->second, g);

    // Check for cycles
    bool has_cycle = false;
    int src = -1;
    cycle_detector vis(has_cycle, src);
    depth_first_search(g, visitor(vis));

    if (has_cycle) {
        std::string s =  revNodes[src].toString() + " reference creates a cyclic dependency.";

        throw Base::RuntimeError(s.c_str());
    }
}

/**
 * The code below builds a graph for all expressions in the engine, and
 * finds any circular dependencies. It also computes the internal evaluation
 * order, in case properties depends on each other.
 */

std::vector<App::ObjectIdentifier> PropertyExpressionEngine::computeEvaluationOrder(int output)
{
    std::vector<App::ObjectIdentifier> evaluationOrder;
    boost::unordered_map<int, ObjectIdentifier> revNodes;
    DiGraph g;

    buildGraph(expressions, revNodes, g, output);

    /* Compute evaluation order for expressions */
    std::vector<int> c;
    topological_sort(g, std::back_inserter(c));

    for (std::vector<int>::iterator i = c.begin(); i != c.end(); ++i) {
        if (revNodes.find(*i) != revNodes.end())
            evaluationOrder.push_back(revNodes[*i]);
    }

    return evaluationOrder;
}

/**
 * @brief Compute and update values of all registered experssions.
 * @return StdReturn on success.
 */

DocumentObjectExecReturn *App::PropertyExpressionEngine::execute(int output)
{
    DocumentObject * docObj = freecad_dynamic_cast<DocumentObject>(getContainer());

    if (!docObj)
        throw Base::RuntimeError("PropertyExpressionEngine must be owned by a DocumentObject.");

    if (running)
        return DocumentObject::StdReturn;

    /* Resetter class, to ensure that the "running" variable gets set to false, even if
     * an exception is thrown.
     */

    class resetter {
    public:
        resetter(bool & b) : _b(b) { _b = true; }
        ~resetter() { _b = false; }

    private:
        bool & _b;
    };

    resetter r(running);

    // Compute evaluation order
    std::vector<App::ObjectIdentifier> evaluationOrder = computeEvaluationOrder(output);
    std::vector<ObjectIdentifier>::const_iterator it = evaluationOrder.begin();

#ifdef FC_PROPERTYEXPRESSIONENGINE_LOG
    std::clog << "Computing expressions for " << getName() << std::endl;
#endif

    /* Evaluate the expressions, and update properties */
    while (it != evaluationOrder.end()) {

        // Get property to update
        Property * prop = it->getProperty();

        if (!prop)
            throw Base::RuntimeError("Path does not resolve to a property.");

        DocumentObject* parent = freecad_dynamic_cast<DocumentObject>(prop->getContainer());

        /* Make sure property belongs to the same container as this PropertyExpressionEngine */
        if (parent != docObj)
            throw Base::RuntimeError("Invalid property owner.");

        // Evaluate expression
        std::unique_ptr<Expression> e(expressions[*it].expression->eval());

        /* Set value of property */
        prop->setPathValue(*it, e->getValueAsAny());

        ++it;
    }
    return DocumentObject::StdReturn;
}

/**
 * @brief Find document objects that the expressions depend on.
 * @param docObjs Dependencies
 */

void PropertyExpressionEngine::getDocumentObjectDeps(std::vector<DocumentObject *> &docObjs) const
{
    DocumentObject * owner = freecad_dynamic_cast<DocumentObject>(getContainer());

    if (owner == 0)
        return;

    for(auto &v : expressions) {
        for(auto docObj : v.second.expression->getDepObjects()) {
            if (docObj && docObj != owner)
                docObjs.push_back(docObj);
        }
    }
}

/**
 * @brief Find paths to document object.
 * @param obj Document object
 * @param paths Object identifier
 */

void PropertyExpressionEngine::getPathsToDocumentObject(DocumentObject* obj,
                                 std::vector<App::ObjectIdentifier> & paths) const
{
    DocumentObject * owner = freecad_dynamic_cast<DocumentObject>(getContainer());

    if (owner == 0 || owner==obj)
        return;

    for(auto &v : expressions) {
        const auto &deps = v.second.expression->getDeps();
        auto it = deps.find(obj);
        if(it==deps.end())
            continue;
        for(auto &dep : it->second) 
            paths.insert(paths.end(),dep.second.begin(),dep.second.end());
    }
}

/**
 * @brief Determine whether any dependencies of any of the registered expressions have been touched.
 * @return True if at least on dependency has been touched.
 */

bool PropertyExpressionEngine::depsAreTouched() const
{
    for(auto &v : expressions)
        if(v.second.expression->isTouched())
            return true;
    return false;
}

/**
 * @brief Get a map of all registered expressions.
 * @return Map of expressions.
 */

boost::unordered_map<const ObjectIdentifier, const PropertyExpressionEngine::ExpressionInfo> PropertyExpressionEngine::getExpressions() const
{
    boost::unordered_map<const ObjectIdentifier, const ExpressionInfo> result;

    ExpressionMap::const_iterator i = expressions.begin();
    while (i != expressions.end()) {
        result.insert(std::make_pair(i->first, i->second));
        ++i;
    }

    return result;
}

/**
 * @brief Validate the given path and expression.
 * @param path Object Identifier for expression.
 * @param expr Expression tree.
 * @return Empty string on success, error message on failure.
 */

std::string PropertyExpressionEngine::validateExpression(const ObjectIdentifier &path, boost::shared_ptr<const Expression> expr) const
{
    std::string error;
    ObjectIdentifier usePath(canonicalPath(path));

    if (validator) {
        error = validator(usePath, expr);
        if (error.size() > 0)
            return error;
    }

    // Get document object
    DocumentObject * pathDocObj = usePath.getDocumentObject();
    assert(pathDocObj);

    auto inList = pathDocObj->getInListEx(true);
    for(auto docObj : expr->getDepObjects()) {
        if(inList.count(docObj)) {
            std::stringstream ss;
            ss << "cyclic reference to " << objName(docObj);
            return ss.str();
        }
    }

    // Check for internal document object dependencies

    // Copy current expressions
    ExpressionMap newExpressions = expressions;

    // Add expression in question
    boost::shared_ptr<Expression> exprClone(expr->copy());
    newExpressions[usePath].expression = exprClone;

    // Build graph; an exception will be thrown if it is not a DAG
    try {
        boost::unordered_map<int, ObjectIdentifier> revNodes;
        DiGraph g;

        buildGraph(newExpressions, revNodes, g);
    }
    catch (const Base::Exception & e) {
        return e.what();
    }

    return std::string();
}

/**
 * @brief Rename paths based on \a paths.
 * @param paths Map with current and new object identifier.
 */

void PropertyExpressionEngine::renameExpressions(const std::map<ObjectIdentifier, ObjectIdentifier> & paths)
{
    ExpressionMap newExpressions;
    std::map<ObjectIdentifier, ObjectIdentifier> canonicalPaths;

    /* ensure input map uses canonical paths */
    for (std::map<ObjectIdentifier, ObjectIdentifier>::const_iterator i = paths.begin(); i != paths.end(); ++i)
        canonicalPaths[canonicalPath(i->first)] = i->second;

    for (ExpressionMap::const_iterator i = expressions.begin(); i != expressions.end(); ++i) {
        std::map<ObjectIdentifier, ObjectIdentifier>::const_iterator j = canonicalPaths.find(i->first);

        // Renamed now?
        if (j != canonicalPaths.end())
            newExpressions[j->second] = i->second;
        else
            newExpressions[i->first] = i->second;
    }

    aboutToSetValue();
    expressions = newExpressions;
    for (ExpressionMap::const_iterator i = expressions.begin(); i != expressions.end(); ++i) 
        expressionChanged(i->first);
    
    hasSetValue();
}

/**
 * @brief Rename object identifiers in the registered expressions.
 * @param paths Map with current and new object identifiers.
 */

void PropertyExpressionEngine::renameObjectIdentifiers(const std::map<ObjectIdentifier, ObjectIdentifier> &paths)
{
    for (ExpressionMap::iterator it = expressions.begin(); it != expressions.end(); ++it) {
        RenameObjectIdentifierExpressionVisitor<PropertyExpressionEngine> v(*this, paths, it->first);
        it->second.expression->visit(v);
    }
}

PyObject *PropertyExpressionEngine::getPyObject(void)
{
    Py::List list;
    for (ExpressionMap::const_iterator it = expressions.begin(); it != expressions.end(); ++it) {
        Py::Tuple tuple(2);
        tuple.setItem(0, Py::String(it->first.toString()));
        tuple.setItem(1, Py::String(it->second.expression->toString()));
        list.append(tuple);
    }
    return Py::new_reference_to(list);
}

void PropertyExpressionEngine::setPyObject(PyObject *)
{
    throw Base::RuntimeError("Property is read-only");
}

void PropertyExpressionEngine::breakDependency(const std::vector<DocumentObject*> &objs) {
    std::vector<DocumentObject*> deps;
    getDocumentObjectDeps(deps);
    std::set<DocumentObject*> depSet(deps.begin(),deps.end());
    for (auto obj : objs) {
        if (depSet.find(obj)!=depSet.end()) {
            std::vector<App::ObjectIdentifier> paths;
            getPathsToDocumentObject(obj, paths);
            for(auto &id : paths)
                setValue(id,nullptr);
        }
    }
}

bool PropertyExpressionEngine::adjustLinks(const std::set<DocumentObject*> &inList) {
    std::unique_ptr<AtomicPropertyChange> signaler;

    auto owner = dynamic_cast<App::DocumentObject*>(getContainer());
    if(!owner)
        return false;
        for(auto &v : expressions) {
        if(!v.second.expression)
            continue;
        try {
            bool need_adjust = false;
            auto depObjs = v.second.expression->getDepObjects();
            for(auto docObj : depObjs) {
                if (docObj && docObj != owner && inList.count(docObj)) {
                    need_adjust = true;
                    break;
                }
            }
            if(!need_adjust)
                continue;
            if(!signaler)
                signaler.reset(new AtomicPropertyChange(*this));
            for(auto obj : depObjs)
                obj->_removeBackLink(owner);

            v.second.expression->adjustLinks(inList);

            for(auto docObj : v.second.expression->getDepObjects()) {
                if (docObj && (docObj != owner))
                    docObj->_addBackLink(owner);
            }

            expressionChanged(v.first);
        }catch(Base::Exception &e) {
            std::ostringstream ss;
            ss << "Failed to adjust link for " << objName(owner) << " in expression "
                << v.second.expression->toString() << ": " << e.what();
            throw Base::RuntimeError(ss.str());
        }
    }
    return !!signaler;
}
