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
        /// <summary>The string value of a member, or null when it is missing or not a string.</summary>
        public static string Str(JToken token)
        {
            return token != null && token.Type == JTokenType.String ? token.Value<string>() : null;
        }

        /// <summary>The integer value of a member, or null when it is missing or not an integer.</summary>
        public static long? Long(JToken token)
        {
            return token != null && token.Type == JTokenType.Integer ? token.Value<long>() : (long?)null;
        }

        public static bool Bool(JToken token)
        {
            return token != null && token.Type == JTokenType.Boolean && token.Value<bool>();
        }

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

        /// <summary>WP-35: where the first compiler error is (absolute path), when the broker says so; null otherwise.</summary>
        public string File { get; set; }

        /// <summary>1-based, or null.</summary>
        public long? Line { get; set; }

        /// <summary>1-based, or null (cl prints no column unless asked to).</summary>
        public long? Column { get; set; }

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
            response.Id = Json.Long(obj["id"]) ?? -1;
            response.Ok = Json.Bool(obj["ok"]);
            response.Result = obj["result"] as JObject ?? new JObject();
            var diag = obj["diag"] as JObject;
            if (diag != null)
            {
                response.Error = new BrokerError
                {
                    Id = Json.Str(diag["id"]) ?? "",
                    Message = Json.Str(diag["message"]) ?? "",
                    Action = Json.Str(diag["action"]) ?? "",
                    File = Json.Str(diag["file"]),
                    Line = Json.Long(diag["line"]),
                    Column = Json.Long(diag["column"]),
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

        /// <summary>{"event":"restarting","old_pid":N,"new_pid":M} (WP-34): the new process exists and is held suspended until `restart-resume`.</summary>
        public bool TryGetRestarting(out int oldPid, out int newPid)
        {
            return TryGetPids("restarting", out oldPid, out newPid);
        }

        /// <summary>{"event":"restarted","old_pid":N,"new_pid":M}</summary>
        public bool TryGetRestarted(out int oldPid, out int newPid)
        {
            return TryGetPids("restarted", out oldPid, out newPid);
        }

        private bool TryGetPids(string eventName, out int oldPid, out int newPid)
        {
            oldPid = 0;
            newPid = 0;
            if (Name != eventName)
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
            var info = new StatusInfo { Version = Json.Str(result["version"]) ?? "" };
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
                    long? pid = po != null ? Json.Long(po["pid"]) : null;
                    if (!pid.HasValue || pid.Value <= 0 || pid.Value > int.MaxValue)
                    {
                        continue;
                    }

                    var ps = new ProcessStatus { Pid = (int)pid.Value };
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
                                Path = Json.Str(mo["path"]) ?? "",
                                Enabled = Json.Bool(mo["enabled"]),
                                IndexState = Json.Str((mo["index"] as JObject)?["state"]) ?? "",
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

        /// <summary>WP-34: `restart {hold_new}` creates the new process suspended; `restarting` announces it; `restart-resume` lets it run.</summary>
        public const string RestartHold = "restart-hold";
        public const string Revert = "revert";
        public const string Exceptions = "exceptions";
    }
}
