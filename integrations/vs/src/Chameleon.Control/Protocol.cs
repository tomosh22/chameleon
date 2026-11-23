using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using Newtonsoft.Json;
using Newtonsoft.Json.Linq;

namespace Chameleon.Control
{
    /// <summary>JSON helpers: the broker's text is taken as it is (no date or float conversion).</summary>
    public static class Json
    {
        public static JObject ParseObject(string text)
        {
            using (var reader = new JsonTextReader(new StringReader(text)))
            {
                reader.DateParseHandling = DateParseHandling.None;
                reader.FloatParseHandling = FloatParseHandling.Double;
                reader.MaxDepth = 64;
                JToken token = JToken.ReadFrom(reader);
                var obj = token as JObject;
                if (obj == null)
                {
                    throw new JsonException("not a JSON object");
                }

                return obj;
            }
        }
    }

    /// <summary>A diagnostic the broker sent in an error response: {"id":"CMLnnnn","message":..,"action":..}.</summary>
    public sealed class BrokerError
    {
        public string Id { get; set; }
        public string Message { get; set; }
        public string Action { get; set; }

        public override string ToString()
        {
            return Id + ": " + Message + (string.IsNullOrEmpty(Action) ? "" : " What to do: " + Action);
        }
    }

    /// <summary>One answer line: {"id":N,"ok":true,"result":{...}} or {"id":N,"ok":false,"diag":{...}}.</summary>
    public sealed class ControlResponse
    {
        public long Id { get; set; }
        public bool Ok { get; set; }
        public JObject Result { get; set; }
        public BrokerError Error { get; set; }

        public static ControlResponse Parse(JObject obj)
        {
            var response = new ControlResponse();
            JToken id = obj["id"];
            response.Id = id != null && id.Type == JTokenType.Integer ? id.Value<long>() : -1;
            JToken ok = obj["ok"];
            response.Ok = ok != null && ok.Type == JTokenType.Boolean && ok.Value<bool>();
            response.Result = obj["result"] as JObject ?? new JObject();
            var diag = obj["diag"] as JObject;
            if (diag != null)
            {
                response.Error = new BrokerError
                {
                    Id = (string)diag["id"] ?? "",
                    Message = (string)diag["message"] ?? "",
                    Action = (string)diag["action"] ?? "",
                };
            }
            else if (!response.Ok)
            {
                response.Error = new BrokerError { Id = "", Message = "the broker answered with an error and no diagnostic", Action = "" };
            }

            return response;
        }
    }

    /// <summary>An unsolicited line: {"event":"diag", ...}. The body keeps every member the broker sent.</summary>
    public sealed class BrokerEvent
    {
        public BrokerEvent(string name, JObject body)
        {
            Name = name;
            Body = body;
        }

        public string Name { get; }
        public JObject Body { get; }

        public long? Int(string key)
        {
            JToken t = Body[key];
            if (t == null)
            {
                return null;
            }

            if (t.Type == JTokenType.Integer)
            {
                return t.Value<long>();
            }

            return null;
        }

        public string Str(string key)
        {
            JToken t = Body[key];
            return t != null && t.Type == JTokenType.String ? t.Value<string>() : null;
        }

        /// <summary>{"event":"restarted","old_pid":N,"new_pid":M}</summary>
        public bool TryGetRestarted(out int oldPid, out int newPid)
        {
            oldPid = 0;
            newPid = 0;
            if (Name != "restarted")
            {
                return false;
            }

            long? o = Int("old_pid");
            long? n = Int("new_pid");
            if (!o.HasValue || !n.HasValue || o.Value <= 0 || n.Value <= 0 || o.Value > int.MaxValue || n.Value > int.MaxValue)
            {
                return false;
            }

            oldPid = (int)o.Value;
            newPid = (int)n.Value;
            return true;
        }
    }

    public sealed class ModuleStatus
    {
        public string Path { get; set; }
        public bool Enabled { get; set; }

        /// <summary>The state of the module's PDB index ("loading", "ready", "failed", "pending"); empty when the broker reports none.</summary>
        public string IndexState { get; set; } = "";
    }

    public sealed class ProcessStatus
    {
        public int Pid { get; set; }
        public List<ModuleStatus> Modules { get; } = new List<ModuleStatus>();
    }

    /// <summary>The result of `status`: version, the exact list of capabilities, and the connected processes.</summary>
    public sealed class StatusInfo
    {
        public string Version { get; set; } = "";
        public List<string> Capabilities { get; } = new List<string>();
        public List<ProcessStatus> Processes { get; } = new List<ProcessStatus>();

        public bool Has(string capability)
        {
            return Capabilities.Contains(capability);
        }

        public static StatusInfo FromResult(JObject result)
        {
            var info = new StatusInfo { Version = (string)result["version"] ?? "" };
            var caps = result["capabilities"] as JArray;
            if (caps != null)
            {
                foreach (JToken c in caps)
                {
                    if (c.Type == JTokenType.String)
                    {
                        info.Capabilities.Add(c.Value<string>());
                    }
                }
            }

            var processes = result["processes"] as JArray;
            if (processes != null)
            {
                foreach (JToken p in processes)
                {
                    var po = p as JObject;
                    if (po == null || po["pid"] == null || po["pid"].Type != JTokenType.Integer)
                    {
                        continue;
                    }

                    var ps = new ProcessStatus { Pid = (int)po["pid"].Value<long>() };
                    var modules = po["modules"] as JArray;
                    if (modules != null)
                    {
                        foreach (JToken m in modules)
                        {
                            var mo = m as JObject;
                            if (mo == null)
                            {
                                continue;
                            }

                            ps.Modules.Add(new ModuleStatus
                            {
                                Path = (string)mo["path"] ?? "",
                                Enabled = mo["enabled"] != null && mo["enabled"].Type == JTokenType.Boolean && mo["enabled"].Value<bool>(),
                                IndexState = (mo["index"] as JObject != null ? (string)mo["index"]["state"] : null) ?? "",
                            });
                        }
                    }

                    info.Processes.Add(ps);
                }
            }

            return info;
        }

        public override string ToString()
        {
            return string.Format(CultureInfo.InvariantCulture, "broker {0}, {1} process(es), capabilities: {2}", Version, Processes.Count, string.Join(",", Capabilities));
        }
    }

    /// <summary>Names of the control capabilities the extension uses (ADR-0032).</summary>
    public static class Capabilities
    {
        public const string Status = "status";
        public const string Reload = "reload";
        public const string Subscribe = "subscribe";
        public const string Restart = "restart";
        public const string Revert = "revert";
        public const string Exceptions = "exceptions";
    }
}
