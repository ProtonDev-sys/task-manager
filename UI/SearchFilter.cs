using System.Globalization;
using TaskManager.Monitoring;

namespace TaskManager.UI;

internal static class SearchFilter
{
    public static bool Matches(ProcessSample process, ProcessMetadata? metadata, string query) =>
        Contains(process.Name, query) || Contains(process.Id.ToString(CultureInfo.InvariantCulture), query) ||
        Contains(metadata?.DisplayName, query) || Contains(metadata?.Publisher, query) ||
        Contains(metadata?.Description, query) || Contains(metadata?.PackageName, query);

    public static bool Contains(string? value, string query) =>
        query.Length == 0 || value?.Contains(query, StringComparison.OrdinalIgnoreCase) == true;
}
