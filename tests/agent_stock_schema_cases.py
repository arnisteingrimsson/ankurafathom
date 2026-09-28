"""Structural and semantic negative cases for agent_stock_sd."""
import copy
import json


def cases(models):
    base = json.loads((models/"agent_stock_sd.ir.json").read_text())
    structural, semantic = [], []

    def changed(target, mutate):
        document = copy.deepcopy(base)
        mutate(document)
        target.append(document)

    for path in ([], ["time"], ["parameters", 0], ["components", 0], ["components", 0, "fields", 0],
                 ["components", 1], ["components", 3], ["components", 11], ["components", 12], ["outputs", 0]):
        def extra(d, path=path):
            node = d
            for key in path:
                node = node[key]
            node["unsupported"] = True
        changed(structural, extra)
    for path, key in (([], "mode"), (["components", 0], "execution"), (["components", 0], "fields"),
                      (["components", 0], "agents"), (["components", 1], "field"),
                      (["components", 1], "population"), (["components", 3], "expr"),
                      (["components", 3], "unit"), (["outputs", 0], "expr"), (["outputs", 0], "unit")):
        def remove(d, path=path, key=key):
            node = d
            for part in path:
                node = node[part]
            del node[key]
        changed(structural, remove)
    changed(structural, lambda d: d["components"][1].pop("outflow_expr"))
    for index, key, value in [(0,"execution","sync"),(0,"dt",0),(0,"fields",[]),
                              (1,"non_negative",1),(3,"op","median"),(3,"expr",[]),
                              (3,"empty_value",0),(6,"empty_value",None),(7,"expr","todo"),
                              (12,"source",None)]:
        if index == 12:
            def null_endpoints(d):
                d["components"][12]["source"] = None
                d["components"][12]["destination"] = None
            changed(structural, null_endpoints)
        else:
            changed(structural, lambda d, i=index, k=key, v=value: d["components"][i].__setitem__(k,v))
    changed(structural, lambda d: d.__setitem__("outputs", []))
    changed(structural, lambda d: d["outputs"][0].__setitem__("expr", ""))
    changed(structural, lambda d: d.__setitem__("integrator", "euler"))
    changed(structural, lambda d: d.__setitem__("links", []))

    changed(semantic, lambda d: d["components"].append(copy.deepcopy(d["components"][0])))
    changed(semantic, lambda d: d["components"].pop(0))
    changed(semantic, lambda d: d["components"][0].__setitem__("dt",1e-12))
    changed(semantic, lambda d: d["components"][0]["fields"][0].__setitem__("name","productivity"))
    changed(semantic, lambda d: d["components"][0]["fields"][3].__setitem__("unit","hour"))
    for key, value in [("todo",True),("todo",-1),("level",1.5),("level",9007199254740992),("extra",1)]:
        changed(semantic, lambda d,k=key,v=value: d["components"][0]["agents"][0].__setitem__(k,v))
    for index, key, value in [(1,"field","level"),(2,"field","todo"),(1,"population","missing"),
                              (3,"population","missing"),(1,"outflow_expr","todo"),
                              (1,"outflow_expr","throughput"),(3,"expr","role"),(3,"expr","t"),
                              (3,"expr","todo +"),(3,"unit","dollar"),(7,"unit","person"),
                              (6,"filter","todo"),(12,"destination","pending"),(12,"expr","todo"),
                              (12,"unit","hour"),(3,"id","productivity")]:
        changed(semantic, lambda d,i=index,k=key,v=value: d["components"][i].__setitem__(k,v))
    changed(semantic, lambda d: d["outputs"][0].__setitem__("unit","day"))
    changed(semantic, lambda d: d["outputs"][0].__setitem__("id","pending"))
    return ([(f"agent_stock_structural_{i}", case) for i, case in enumerate(structural)],
            [(f"agent_stock_semantic_{i}", case) for i, case in enumerate(semantic)])
